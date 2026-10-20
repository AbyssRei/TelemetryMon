#pragma once

#include <cstdint>
#include <string>

// Output format for captured frame images.
enum class FrameCaptureFormat {
    Png,
    Bmp,
};

// Screencap method bits, matching MaaWin32Screencap's ScreencapMethod values
// (see MaaWin32Screencap/include/MaaWin32Screencap/ScreencapMgr.h).  The mask
// selects which built-in capture methods participate in MaaWin32Screencap's
// warmup + speed test; the fastest working one is used.
enum FrameCaptureMethod : uint64_t {
    FrameCaptureMethod_GDI                   = 1ull << 0,
    FrameCaptureMethod_FramePool             = 1ull << 1,  // Windows.Graphics.Capture
    FrameCaptureMethod_DXGI_DesktopDup       = 1ull << 2,  // entire desktop
    FrameCaptureMethod_DXGI_DesktopDupWindow = 1ull << 3,  // desktop dup, window crop
    FrameCaptureMethod_PrintWindow           = 1ull << 4,
    FrameCaptureMethod_ScreenDC              = 1ull << 5,
    FrameCaptureMethod_Foreground            = FrameCaptureMethod_DXGI_DesktopDupWindow | FrameCaptureMethod_ScreenDC,
    FrameCaptureMethod_Background            = FrameCaptureMethod_FramePool | FrameCaptureMethod_PrintWindow,
    FrameCaptureMethod_All                   = ~0ull,
};

struct FrameCaptureOptions {
    wchar_t const* rootDir = nullptr;       // Session root; nullptr = current directory
    uint32_t captureEvery = 1;              // Capture every nth frame
    double intervalSeconds = 0;             // Wall-clock throttle; 0 = off
    uint64_t methodMask = FrameCaptureMethod_All;
    FrameCaptureFormat format = FrameCaptureFormat::Png;
};

// A request to capture the image of one frame.  The captured image is the
// window's on-screen content at capture time, which is a few milliseconds
// after the present.  hwnd 0 means the frame carried no window handle; the
// largest visible top-level window of the process is then used, falling back
// to the entire desktop when it has none.
struct FrameCaptureJob {
    uint64_t hwnd = 0;
    uint32_t processId = 0;
    uint32_t frameId = 0;
    uint64_t swapChainAddress = 0;
    uint64_t presentQpc = 0;
    double presentTimeSeconds = 0.;
    std::wstring processName;
};

// FrameCapture.cpp:
// Creates <root>\capture_<date>T<time>\Frames, starts the capture thread, and
// opens frame_manifest.csv inside it.  Returns false (capture disabled for
// the session) on directory/manifest/thread failure.
bool FrameCaptureStart(FrameCaptureOptions const& options);
// Drains the queue, restores windows, prints statistics.
void FrameCaptureStop();
// Queues a job for the capture thread (bounded queue, drop-newest).
void FrameCaptureEnqueue(FrameCaptureJob&& job);
// Directory of the current capture session, nullptr when not running.
wchar_t const* FrameCaptureSessionDir();
