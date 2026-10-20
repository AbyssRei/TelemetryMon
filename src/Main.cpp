#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include "FrameCapture.h"
#include "PmClient.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

namespace {

bool gQuit = false;

BOOL WINAPI HandleCtrlEvent(DWORD)
{
    gQuit = true;
    // Give the main loop time to shut down cleanly; the handler thread can
    // just sleep (same approach as PresentMon's console app).
    Sleep(5000);
    return TRUE;
}

void PrintUsage()
{
    fwprintf(stderr, L"TelemetryMon - per-frame metrics + GPU/CPU telemetry + frame images,\n"
                     L"              consuming the running PresentMon shared service.\n\n");
    fwprintf(stderr, L"  -process_name <name>            Target process by exe name (waited for until it appears).\n");
    fwprintf(stderr, L"  -process_id <pid>               Target process by id (skips the name lookup).\n");
    fwprintf(stderr, L"  -timed <seconds>                Stop after the specified amount of time.\n");
    fwprintf(stderr, L"  -telemetry_period <ms>          Hardware telemetry poll rate, 50-5000 (default 50).\n\n");
    fwprintf(stderr, L"  -no_capture_frames              Disable frame image capture (default: on).\n");
    fwprintf(stderr, L"  -capture_frames_dir <path>      Session root directory (default: current directory).\n");
    fwprintf(stderr, L"  -capture_frames_every <n>       Capture every nth frame (default 1).\n");
    fwprintf(stderr, L"  -capture_frames_interval <s>    Capture at most one frame per interval (wall-clock).\n");
    fwprintf(stderr, L"  -capture_frames_method <m>      all (default), foreground, background, or a bitmask.\n");
    fwprintf(stderr, L"  -capture_frames_format <f>      png (default) or bmp.\n");
}

struct Args {
    std::wstring processName;
    uint32_t processId = 0;
    double timedSeconds = 0;
    uint32_t telemetryPeriodMs = 50;
    bool captureFrames = true;
    bool allColumns = false;            // full GUI layout with NA instead of trimming
    FrameCaptureOptions capture;
};

// Canonicalize an exe name for comparison: strip directories and the .exe
// extension (case-insensitive comparison happens separately).
std::wstring CanonicalizeProcessName(std::wstring s)
{
    auto pos = s.find_last_of(L"\\/");
    if (pos != std::wstring::npos) s = s.substr(pos + 1);
    if (s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".exe") == 0) s.resize(s.size() - 4);
    return s;
}

bool ParseArgs(int argc, wchar_t** argv, Args* args)
{
    for (int i = 1; i < argc; ++i) {
        auto arg = argv[i];
        if (arg[0] == L'-' || arg[0] == L'/') {
            arg += 1 + (arg[1] == L'-');
        }
        auto const next = [&]() -> wchar_t const* {
            if (i + 1 >= argc) {
                fwprintf(stderr, L"error: -%s expecting an argument.\n", arg);
                return nullptr;
            }
            return argv[++i];
        };

        if (_wcsicmp(arg, L"process_name") == 0) {
            if (auto v = next()) args->processName = v;
            else return false;
        } else if (_wcsicmp(arg, L"process_id") == 0) {
            if (auto v = next()) args->processId = wcstoul(v, nullptr, 10);
            else return false;
        } else if (_wcsicmp(arg, L"timed") == 0) {
            if (auto v = next()) args->timedSeconds = wcstod(v, nullptr);
            else return false;
        } else if (_wcsicmp(arg, L"telemetry_period") == 0) {
            if (auto v = next()) args->telemetryPeriodMs = wcstoul(v, nullptr, 10);
            else return false;
        } else if (_wcsicmp(arg, L"capture_frames") == 0) {
            args->captureFrames = true;
        } else if (_wcsicmp(arg, L"no_capture_frames") == 0) {
            args->captureFrames = false;
        } else if (_wcsicmp(arg, L"all_columns") == 0) {
            args->allColumns = true;
        } else if (_wcsicmp(arg, L"capture_frames_dir") == 0) {
            if (auto v = next()) args->capture.rootDir = v; // argv storage outlives the run
            else return false;
        } else if (_wcsicmp(arg, L"capture_frames_every") == 0) {
            if (auto v = next()) args->capture.captureEvery = wcstoul(v, nullptr, 10);
            else return false;
        } else if (_wcsicmp(arg, L"capture_frames_interval") == 0) {
            if (auto v = next()) args->capture.intervalSeconds = wcstod(v, nullptr);
            else return false;
        } else if (_wcsicmp(arg, L"capture_frames_method") == 0) {
            if (auto v = next()) {
                if (_wcsicmp(v, L"all") == 0)        args->capture.methodMask = FrameCaptureMethod_All;
                else if (_wcsicmp(v, L"foreground") == 0) args->capture.methodMask = FrameCaptureMethod_Foreground;
                else if (_wcsicmp(v, L"background") == 0) args->capture.methodMask = FrameCaptureMethod_Background;
                else args->capture.methodMask = wcstoull(v, nullptr, 0);
                if (args->capture.methodMask == 0) {
                    fwprintf(stderr, L"error: invalid -capture_frames_method '%s'.\n", v);
                    return false;
                }
            } else return false;
        } else if (_wcsicmp(arg, L"capture_frames_format") == 0) {
            if (auto v = next()) {
                if (_wcsicmp(v, L"png") == 0) args->capture.format = FrameCaptureFormat::Png;
                else if (_wcsicmp(v, L"bmp") == 0) args->capture.format = FrameCaptureFormat::Bmp;
                else {
                    fwprintf(stderr, L"error: invalid -capture_frames_format '%s'.\n", v);
                    return false;
                }
            } else return false;
        } else if (_wcsicmp(arg, L"?") == 0 || _wcsicmp(arg, L"h") == 0 || _wcsicmp(arg, L"help") == 0) {
            PrintUsage();
            exit(0);
        } else {
            fwprintf(stderr, L"error: unrecognized option '%s'.\n\n", argv[i]);
            PrintUsage();
            return false;
        }
    }

    if (args->processName.empty() && args->processId == 0) {
        PrintUsage();
        return false;
    }
    // Normalize once so that process lookup, CSV output, and capture jobs all
    // use the same bare exe name (no directory, no .exe extension).
    if (!args->processName.empty()) {
        args->processName = CanonicalizeProcessName(args->processName);
    }
    return true;
}

// Wait (up to forever / Ctrl+C) for a process whose exe name matches, and
// return its pid.  Matching ignores case, directories, and the .exe extension
// on both the candidate process name and the requested name.
uint32_t WaitForProcess(std::wstring const& name)
{
    auto const match = [&name](wchar_t const* candidate) {
        return _wcsicmp(CanonicalizeProcessName(candidate).c_str(), name.c_str()) == 0;
    };

    bool printed = false;
    while (!gQuit) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry = {};
            entry.dwSize = sizeof(entry);
            if (Process32FirstW(snap, &entry)) {
                do {
                    if (match(entry.szExeFile)) {
                        CloseHandle(snap);
                        return entry.th32ProcessID;
                    }
                } while (Process32NextW(snap, &entry));
            }
            CloseHandle(snap);
        }
        if (!printed) {
            wprintf(L"Waiting for process '%s'...\n", name.c_str());
            fflush(stdout);
            printed = true;
        }
        Sleep(500);
    }
    return 0;
}

// Decode one column of a frame blob into its CSV text form.
void WriteColumnValue(FILE* fp, uint8_t const* blob, FrameColumn const& col)
{
    auto const* p = blob + col.offset;
    switch (col.type) {
    case PM_DATA_TYPE_DOUBLE:
        fwprintf(fp, L"%.4lf", *reinterpret_cast<double const*>(p));
        break;
    case PM_DATA_TYPE_INT32:
        fwprintf(fp, L"%d", *reinterpret_cast<int32_t const*>(p));
        break;
    case PM_DATA_TYPE_UINT32:
        fwprintf(fp, L"%u", *reinterpret_cast<uint32_t const*>(p));
        break;
    case PM_DATA_TYPE_UINT64:
        fwprintf(fp, L"%llu", (unsigned long long) *reinterpret_cast<uint64_t const*>(p));
        break;
    case PM_DATA_TYPE_BOOL:
        fwprintf(fp, L"%d", *reinterpret_cast<uint8_t const*>(p) ? 1 : 0);
        break;
    case PM_DATA_TYPE_ENUM:
    {
        auto const v = *reinterpret_cast<int32_t const*>(p);
        if (col.metric == PM_METRIC_PRESENT_MODE)          fwprintf(fp, L"%hs", TranslatePresentMode(v));
        else if (col.metric == PM_METRIC_PRESENT_RUNTIME)  fwprintf(fp, L"%hs", TranslateRuntime(v));
        else if (col.metric == PM_METRIC_FRAME_TYPE)       fwprintf(fp, L"%hs", TranslateFrameType(v));
        else                                               fwprintf(fp, L"%d", v);
        break;
    }
    default:
        fwprintf(fp, L"NA");
        break;
    }
}

std::wstring CsvFilePathFromSessionDir()
{
    // <session>\capture_<ts> -> <session>\PresentMon-<ts>.csv
    std::wstring dir = FrameCaptureSessionDir();
    auto pos = dir.find_last_of(L"\\/");
    std::wstring leaf = pos == std::wstring::npos ? dir : dir.substr(pos + 1);
    if (leaf.rfind(L"capture_", 0) == 0) {
        leaf = leaf.substr(8);
    }
    return dir + L"\\PresentMon-" + leaf + L".csv";
}

}

int wmain(int argc, wchar_t** argv)
{
    Args args;
    if (!ParseArgs(argc, argv, &args)) {
        return 1;
    }

    SetConsoleCtrlHandler(HandleCtrlEvent, TRUE);

    PmClient client;
    if (!client.Init()) {
        return 1;
    }
    client.SetKeepAllColumns(args.allColumns);
    if (!client.Introspect()) {
        return 1;
    }

    // Start the frame capture thread (also creates the session directory that
    // the CSV file goes into).
    if (args.captureFrames && !FrameCaptureStart(args.capture)) {
        client.Shutdown();
        return 1;
    }

    // Register the frame query before tracking (same order as the SDK's
    // FrameQuerySample).
    if (!client.RegisterFrameQuery()) {
        FrameCaptureStop();
        client.Shutdown();
        return 1;
    }

    // Resolve and track the target process.
    uint32_t pid = args.processId;
    if (pid == 0) {
        pid = WaitForProcess(args.processName);
        if (pid == 0) {
            FrameCaptureStop();
            client.Shutdown();
            return 1;
        }
    }
    if (!client.TrackProcess(pid)) {
        FrameCaptureStop();
        client.Shutdown();
        return 1;
    }
    std::wstring processName = args.processName.empty()
        ? L"pid" + std::to_wstring(pid)
        : args.processName;
    wprintf(L"Tracking process %s (pid %u)\n", processName.c_str(), pid);
    fflush(stdout);

    FILE* csv = nullptr;
    if (args.captureFrames) {
        auto const path = CsvFilePathFromSessionDir();
        if (_wfopen_s(&csv, path.c_str(), L"w,ccs=UTF-8") != 0 || csv == nullptr) {
            fwprintf(stderr, L"error: failed to create metrics CSV '%s'.\n", path.c_str());
            FrameCaptureStop();
            client.Shutdown();
            return 1;
        }
        // Header
        bool first = true;
        for (auto const& col : client.Columns()) {
            if (col.header[0] == '\0') continue;
            fwprintf(csv, L"%s%hs", first ? L"" : L",", col.header);
            first = false;
        }
        fwprintf(csv, L"\n");
    }

    LARGE_INTEGER freq = {};
    QueryPerformanceFrequency(&freq);
    uint64_t qpc0 = 0;
    uint64_t rowsWritten = 0;

    auto const startTime = std::chrono::steady_clock::now();
    wprintf(L"Collecting... (Ctrl+C to stop)\n");

    auto const consumeAll = [&]() {
        for (;;) {
            auto const n = client.ConsumeFrames();
            if (n == 0) {
                return;
            }
            for (uint32_t i = 0; i < n; ++i) {
                auto const* blob = client.FrameBlob(i);

                // Time base: seconds since the first frame of this session.
                uint64_t qpc = 0;
                for (auto const& col : client.Columns()) {
                    if (col.metric == PM_METRIC_CPU_START_QPC) {
                        qpc = *reinterpret_cast<uint64_t const*>(blob + col.offset);
                        break;
                    }
                }
                if (qpc0 == 0 && qpc != 0) {
                    qpc0 = qpc;
                }
                double const timeSeconds = freq.QuadPart > 0 && qpc != 0
                    ? double(qpc - qpc0) / double(freq.QuadPart)
                    : 0.0;

                if (csv != nullptr) {
                    bool first = true;
                    for (auto const& col : client.Columns()) {
                        if (col.header[0] == '\0') continue;
                        fwprintf(csv, L"%s", first ? L"" : L",");
                        first = false;
                        if (col.computed) {
                            switch (col.metric) {
                            case PM_METRIC_APPLICATION: fwprintf(csv, L"%s", processName.c_str()); break;
                            case PM_METRIC_PROCESS_ID:  fwprintf(csv, L"%u", pid); break;
                            default:                    fwprintf(csv, L"%.4lf", timeSeconds); break;
                            }
                        } else if (!col.available) {
                            fwprintf(csv, L"NA");
                        } else {
                            WriteColumnValue(csv, blob, col);
                        }
                    }
                    fwprintf(csv, L"\n");
                    fflush(csv);
                }

                if (args.captureFrames) {
                    FrameCaptureJob job;
                    job.processId = pid;
                    job.frameId = uint32_t(rowsWritten);
                    job.swapChainAddress = [&] {
                        for (auto const& col : client.Columns()) {
                            if (col.metric == PM_METRIC_SWAP_CHAIN_ADDRESS && col.available) {
                                return *reinterpret_cast<uint64_t const*>(blob + col.offset);
                            }
                        }
                        return 0ull;
                    }();
                    job.presentQpc = qpc;
                    job.presentTimeSeconds = timeSeconds;
                    job.processName = processName;
                    FrameCaptureEnqueue(std::move(job));
                }

                rowsWritten += 1;
            }
            if (rowsWritten % 500 < n) {
                wprintf(L"%llu frames collected\n", (unsigned long long) rowsWritten);
            }
        }
    };

    for (;;) {
        if (gQuit) {
            break;
        }
        if (args.timedSeconds > 0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count() >= args.timedSeconds) {
            break;
        }
        consumeAll();
        Sleep(5);
    }

    // Drain whatever the service still has buffered.
    wprintf(L"Stopping, draining frames...\n");
    for (int i = 0; i < 10; ++i) {
        auto const before = rowsWritten;
        consumeAll();
        if (rowsWritten == before) {
            break;
        }
        Sleep(50);
    }

    if (csv != nullptr) {
        fclose(csv);
    }
    FrameCaptureStop();
    client.Shutdown();

    wprintf(L"Done: %llu frame rows written.\n", (unsigned long long) rowsWritten);
    return 0;
}
