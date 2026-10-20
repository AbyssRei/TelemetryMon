#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// Include MaaWin32Screencap headers first, so that <windows.h> is included
// with NOMINMAX defined (its SafeWindows.hpp only sets it when undefined).
// Disable warnings for these external headers.
#pragma warning(push, 0)
#include <MaaWin32Screencap/ScreencapMgr.h>
#include <MaaWin32Screencap/Image.hpp>
#pragma warning(pop)

#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma warning(push, 0)
#include "stb_image_write.h"
#pragma warning(pop)

#include "FrameCapture.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace {

void PrintWarning(wchar_t const* format, ...)
{
    va_list args;
    va_start(args, format);
    vfwprintf(stderr, format, args);
    va_end(args);
}

void PrintError(wchar_t const* format, ...)
{
    va_list args;
    va_start(args, format);
    vfwprintf(stderr, format, args);
    va_end(args);
}

constexpr size_t MaxQueuedJobs = 16;

// Everything below is guarded by gMutex, except:
// - gMgrs/gRgbBuffer are used by the capture thread only,
// - gNumCaptured/gNumFailed are written by the capture thread and read by
//   FrameCaptureStop() after joining it.
bool gStarted = false;
bool gQuit = false;

std::thread gThread;
std::mutex gMutex;
std::condition_variable gCv;
std::deque<FrameCaptureJob> gQueue;
uint32_t gFrameCounter = 0;

std::wstring gDir;
std::wstring gSessionDir;
FILE* gManifest = nullptr;
FrameCaptureFormat gFormat = FrameCaptureFormat::Png;
uint64_t gMethodMask = FrameCaptureMethod_All;
uint32_t gCaptureEvery = 1;
double gIntervalSeconds = 0;
std::chrono::steady_clock::time_point gLastAcceptedTime{};

uint64_t gNumCaptured = 0;
uint64_t gNumDropped = 0;
uint64_t gNumFailed = 0;
uint32_t gSeq = 0;
uint64_t gFirstQpc = 0;

// Capture-thread-only state.
std::map<uint64_t, std::unique_ptr<mws::ScreencapMgr>> gMgrs;
std::map<uint64_t, std::chrono::steady_clock::time_point> gLastConnectAttempt;
std::set<uint64_t> gWarnedNoMethod;
std::vector<uint8_t> gRgbBuffer;
bool gWarnedNoWindow = false;

struct MainWindowEnumData {
    uint32_t pid;
    int minSize; // minimum client width/height, px
    HWND best = nullptr;
    int bestArea = 0;
};

BOOL CALLBACK EnumWindowsCallback(HWND hwnd, LPARAM lParam)
{
    auto* data = reinterpret_cast<MainWindowEnumData*>(lParam);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != data->pid || !IsWindowVisible(hwnd)) {
        return TRUE;
    }

    // Skip tool windows (tooltips, small helpers); message-only windows are
    // not enumerated at all.  UWP app windows would be attributed to
    // ApplicationFrameHost and thus not found here; such processes fall back
    // to capturing the entire desktop.
    if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) {
        return TRUE;
    }

    RECT rc = {};
    if (!GetClientRect(hwnd, &rc)) {
        return TRUE;
    }
    auto const width = rc.right - rc.left;
    auto const height = rc.bottom - rc.top;
    if (width < data->minSize || height < data->minSize) {
        return TRUE;
    }

    auto const area = width * height;
    if (area > data->bestArea) {
        data->bestArea = area;
        data->best = hwnd;
    }
    return TRUE;
}

// Resolve the window to capture for this frame.  Frame metric events carry no
// window handle, so use the largest visible top-level window of the process,
// falling back to the entire desktop (hwnd 0) when none is found.
uint64_t ResolveFrameWindow(FrameCaptureJob const& job)
{
    if (job.hwnd != 0) {
        return job.hwnd;
    }

    MainWindowEnumData data;
    data.pid = job.processId;
    data.minSize = 200;
    EnumWindows(EnumWindowsCallback, reinterpret_cast<LPARAM>(&data));

    auto const hwnd = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(data.best));
    if (hwnd == 0 && !gWarnedNoWindow) {
        gWarnedNoWindow = true;
        PrintWarning(L"warning: process %u has no visible window; capturing the entire desktop.\n", job.processId);
    }
    return hwnd;
}

void StbWriteCallback(void* context, void* data, int size)
{
    auto& out = *static_cast<std::vector<uint8_t>*>(context);
    auto const* bytes = static_cast<uint8_t const*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

// Return the screencap manager for this window, creating and speed-testing it
// on first use.  Returns nullptr while backing off after a failed connect.
mws::ScreencapMgr* EnsureMgr(uint64_t hwnd)
{
    auto ii = gMgrs.find(hwnd);
    if (ii != gMgrs.end()) {
        return ii->second.get();
    }

    // A failed connect runs a full method speed test, so don't retry it for
    // every frame.
    auto now = std::chrono::steady_clock::now();
    auto li = gLastConnectAttempt.find(hwnd);
    if (li != gLastConnectAttempt.end() && now - li->second < std::chrono::seconds(2)) {
        return nullptr;
    }
    gLastConnectAttempt[hwnd] = now;

    auto mgr = std::make_unique<mws::ScreencapMgr>(reinterpret_cast<HWND>(static_cast<uintptr_t>(hwnd)));
    // Window-less capture can only use the entire-desktop duplication method.
    auto const mask = hwnd == 0
        ? gMethodMask & FrameCaptureMethod_DXGI_DesktopDup
        : gMethodMask;
    mgr->set_builtin_methods(mask);
    if (!mgr->connect()) {
        if (gWarnedNoMethod.insert(hwnd).second) {
            PrintWarning(L"warning: frame capture: no working screencap method for window 0x%llX; skipping its frames.\n",
                         hwnd);
        }
        return nullptr;
    }

    auto* mgrPtr = mgr.get();
    gMgrs.emplace(hwnd, std::move(mgr));
    return mgrPtr;
}

void ProcessJob(FrameCaptureJob const& job)
{
    auto const hwnd = ResolveFrameWindow(job);

    auto* mgr = EnsureMgr(hwnd);
    if (mgr == nullptr) {
        gNumFailed += 1;
        return;
    }

    mws::Image image;
    if (!mgr->screencap(image) || image.data == nullptr || image.rows <= 0 || image.cols <= 0 || image.channels() != 3) {
        // The window may be gone or the capture device lost; recreate the
        // manager on the next attempt.
        gMgrs.erase(hwnd);
        gNumFailed += 1;
        return;
    }

    // Measure how far the captured image lags behind the present.
    uint64_t captureQpc = 0;
    QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&captureQpc));
    LARGE_INTEGER freq = {};
    QueryPerformanceFrequency(&freq);
    auto const lagMs = freq.QuadPart > 0
        ? 1000.0 * double(captureQpc - job.presentQpc) / double(freq.QuadPart)
        : 0.0;

    // Convert BGR -> RGB for the encoders.
    gRgbBuffer.resize(size_t(image.rows) * size_t(image.cols) * 3);
    for (int y = 0; y < image.rows; ++y) {
        auto const* src = image.data + size_t(y) * image.step;
        auto* dst = gRgbBuffer.data() + size_t(y) * size_t(image.cols) * 3;
        for (int x = 0; x < image.cols; ++x) {
            dst[x * 3 + 0] = src[x * 3 + 2];
            dst[x * 3 + 1] = src[x * 3 + 1];
            dst[x * 3 + 2] = src[x * 3 + 0];
        }
    }

    // Encode to memory first, then write with a wide-char fopen, so that
    // non-ASCII capture paths work.
    std::vector<uint8_t> encoded;
    bool ok = false;
    wchar_t const* ext = L"png";
    if (gFormat == FrameCaptureFormat::Png) {
        ok = stbi_write_png_to_func(StbWriteCallback, &encoded, image.cols, image.rows, 3, gRgbBuffer.data(),
                                    image.cols * 3) != 0;
    } else {
        ext = L"bmp";
        ok = stbi_write_bmp_to_func(StbWriteCallback, &encoded, image.cols, image.rows, 3, gRgbBuffer.data()) != 0;
    }
    if (!ok || encoded.empty()) {
        gNumFailed += 1;
        return;
    }

    wchar_t seqStr[16] = {};
    swprintf_s(seqStr, L"%08u", gSeq);
    std::wstring const fileName = std::wstring(L"frame_") + seqStr + L"_" + job.processName + L"_" +
                                  std::to_wstring(job.frameId) + L"." + ext;

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, (gDir + L"\\" + fileName).c_str(), L"wb") != 0 || fp == nullptr) {
        gNumFailed += 1;
        return;
    }
    ok = fwrite(encoded.data(), 1, encoded.size(), fp) == encoded.size();
    fclose(fp);
    if (!ok) {
        gNumFailed += 1;
        return;
    }

    if (gManifest != nullptr) {
        fwprintf(gManifest, L"%u,%s,%s,%u,0x%llX,0x%llX,%u,%.4lf,%llu,%d,%d,%hs,%.2lf\n",
                 gSeq,
                 fileName.c_str(),
                 job.processName.c_str(),
                 job.processId,
                 hwnd,
                 job.swapChainAddress,
                 job.frameId,
                 job.presentTimeSeconds,
                 job.presentQpc,
                 image.cols,
                 image.rows,
                 mgr->selected_unit_name().c_str(),
                 lagMs);
        fflush(gManifest);
    }

    gSeq += 1;
    gNumCaptured += 1;
}

void CaptureThreadProc()
{
    SetThreadDescription(GetCurrentThread(), L"TelemetryMon Frame Capture Thread");

    for (;;) {
        FrameCaptureJob job;
        {
            std::unique_lock<std::mutex> lock(gMutex);
            gCv.wait(lock, [] { return gQuit || !gQueue.empty(); });
            if (gQueue.empty()) {
                return; // quit requested and queue drained
            }
            job = std::move(gQueue.front());
            gQueue.pop_front();
        }
        ProcessJob(job);
    }
}

}

wchar_t const* FrameCaptureSessionDir()
{
    // Set before the consume loop starts and cleared after it ends, so no
    // synchronization is needed.  Empty when capture is off.
    return gSessionDir.empty() ? nullptr : gSessionDir.c_str();
}

bool FrameCaptureStart(FrameCaptureOptions const& options)
{
    std::wstring root;
    if (options.rootDir != nullptr) {
        root = options.rootDir;
    } else {
        wchar_t cwd[MAX_PATH] = {};
        GetCurrentDirectoryW(MAX_PATH, cwd);
        root = cwd;
    }

    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        PrintError(L"error: failed to create frame capture directory '%s': %hs\n", root.c_str(), ec.message().c_str());
        return false;
    }

    // Isolate each run in its own timestamped subfolder, so runs don't mix.
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t session[48] = {};
    swprintf_s(session, L"capture_%04u-%02u-%02uT%02u%02u%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring sessionDir = root + L"\\" + session;
    for (int suffix = 1; std::filesystem::exists(sessionDir); ++suffix) {
        sessionDir = root + L"\\" + session + L"_" + std::to_wstring(suffix);
    }
    std::filesystem::create_directories(sessionDir, ec);
    if (ec) {
        PrintError(L"error: failed to create frame capture directory '%s': %hs\n", sessionDir.c_str(),
                   ec.message().c_str());
        return false;
    }

    gSessionDir = sessionDir;

    // Frame images (and their manifest) go one level deeper, in Frames\.
    gDir = sessionDir + L"\\Frames";
    std::filesystem::create_directories(gDir, ec);
    if (ec) {
        PrintError(L"error: failed to create frame capture directory '%s': %hs\n", gDir.c_str(), ec.message().c_str());
        gSessionDir.clear();
        return false;
    }

    auto const manifestPath = gDir + L"\\frame_manifest.csv";
    FILE* manifest = nullptr;
    if (_wfopen_s(&manifest, manifestPath.c_str(), L"w,ccs=UTF-8") != 0 || manifest == nullptr) {
        PrintError(L"error: failed to create frame capture manifest '%s'.\n", manifestPath.c_str());
        gSessionDir.clear();
        return false;
    }
    fwprintf(manifest,
             L"seq,file_name,process_name,process_id,hwnd,swap_chain_address,frame_id,time_in_seconds,present_qpc,"
             L"width,height,method,capture_lag_ms\n");

    gManifest = manifest;
    gFormat = options.format;
    gMethodMask = options.methodMask;
    gCaptureEvery = options.captureEvery == 0 ? 1 : options.captureEvery;
    gIntervalSeconds = options.intervalSeconds;
    gLastAcceptedTime = {};
    gStarted = true;
    gQuit = false;
    gFrameCounter = 0;
    gSeq = 0;
    gFirstQpc = 0;
    gNumCaptured = 0;
    gNumDropped = 0;
    gNumFailed = 0;
    gMgrs.clear();
    gWarnedNoMethod.clear();
    gLastConnectAttempt.clear();
    gWarnedNoWindow = false;

    try {
        gThread = std::thread(CaptureThreadProc);
    } catch (std::system_error const&) {
        PrintError(L"error: failed to start frame capture thread.\n");
        fclose(gManifest);
        gManifest = nullptr;
        gSessionDir.clear();
        gStarted = false;
        return false;
    }

    return true;
}

void FrameCaptureStop()
{
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gStarted) {
            return;
        }
        gQuit = true;
    }
    gCv.notify_all();
    if (gThread.joinable()) {
        gThread.join();
    }

    // Restore any windows the capture helpers modified (e.g. pseudo-minimized
    // windows) and release the capture resources.
    for (auto& pair : gMgrs) {
        pair.second->inactive();
    }
    gMgrs.clear();
    gRgbBuffer.clear();
    gRgbBuffer.shrink_to_fit();

    if (gManifest != nullptr) {
        fclose(gManifest);
        gManifest = nullptr;
    }

    gSessionDir.clear();
    gStarted = false;

    if (gNumDropped > 0 || gNumFailed > 0) {
        PrintWarning(L"warning: frame capture: %llu images captured, %llu dropped (queue full), %llu failed.\n",
                     (unsigned long long) gNumCaptured, (unsigned long long) gNumDropped,
                     (unsigned long long) gNumFailed);
    } else if (gNumCaptured > 0) {
        wprintf(L"Frame capture: %llu images written to %s\n", (unsigned long long) gNumCaptured, gDir.c_str());
    }
}

void FrameCaptureEnqueue(FrameCaptureJob&& job)
{
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gStarted || gQuit) {
        return;
    }

    if (gCaptureEvery > 1 && ++gFrameCounter % gCaptureEvery != 0) {
        return;
    }

    // Wall-clock throttle: capture at most one frame per interval.  Frames
    // that arrive sooner than the interval since the last accepted one are
    // skipped, so the spacing between captures never goes below the request.
    if (gIntervalSeconds > 0) {
        auto now = std::chrono::steady_clock::now();
        if (now - gLastAcceptedTime < std::chrono::duration<double>(gIntervalSeconds)) {
            return;
        }
        gLastAcceptedTime = now;
    }

    if (gQueue.size() >= MaxQueuedJobs) {
        if (gNumDropped == 0) {
            PrintWarning(L"warning: frame capture queue is full; dropping frames. Consider -capture_frames_every or -capture_frames_interval.\n");
        }
        gNumDropped += 1;
        return;
    }

    gQueue.emplace_back(std::move(job));
    gCv.notify_one();
}
