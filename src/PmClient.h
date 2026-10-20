#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "PresentMonAPI.h"

#include <cstdint>
#include <string>
#include <vector>

// One CSV column.  The layout is FIXED to match the GUI capture application's
// 69-column CSV: columns that are unavailable on this machine stay in the
// output filled with NA (exactly like the GUI), and only available ones are
// registered in the frame query.
struct FrameColumn {
    PM_METRIC metric = PM_METRIC_APPLICATION;
    uint32_t arrayIndex = 0;      // for array metrics (GPUFanSpeed[0..3])
    char const* header = "";
    bool gpuScoped = false;       // use the discovered GPU device id
    bool computed = false;        // filled by the writer, not a frame-query element
    bool available = false;       // resolved via introspection; else NA
    uint32_t deviceId = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    PM_DATA_TYPE type = PM_DATA_TYPE_VOID;
};

// Self-contained client of the PresentMon shared service: binds the
// PresentMonAPI2.dll functions at runtime, introspects the metric surface,
// registers a frame query (frame timing + per-frame telemetry), and consumes
// frames into blobs.
class PmClient
{
public:
    ~PmClient();

    bool Init();                       // LoadLibrary + pmOpenSession (+ version log)
    void Shutdown();                   // stop tracking + close session
    // When true, the full GUI 69-column layout is written with NA for
    // unavailable metrics; when false (default), the layout is trimmed to the
    // available columns.  Call before Introspect().
    void SetKeepAllColumns(bool keep);
    bool Introspect();                 // resolve GPU device + column availability
    bool TrackProcess(uint32_t pid);
    bool RegisterFrameQuery();         // register with the available columns
    uint32_t ConsumeFrames();          // returns number of frames now available

    uint32_t FrameCount() const { return framesPopulated_; }
    uint8_t const* FrameBlob(uint32_t index) const
    {
        return frameBlobs_.data() + size_t(index) * frameBlobSize_;
    }
    std::vector<FrameColumn> const& Columns() const { return columns_; }
    uint32_t GpuDeviceId() const { return gpuDeviceId_; }
    std::wstring const& GpuName() const { return gpuName_; }
    uint32_t TrackedPid() const { return pid_; }

private:
    bool BindFunctions();
    PM_INTROSPECTION_METRIC const* FindMetric_(PM_METRIC id) const;

    HMODULE dll_ = nullptr;
    PM_SESSION_HANDLE session_ = nullptr;
    PM_FRAME_QUERY_HANDLE frameQuery_ = nullptr;
    bool keepAllColumns_ = false;
    uint32_t pid_ = 0;
    uint32_t gpuDeviceId_ = 0;
    std::wstring gpuName_;
    uint32_t frameBlobSize_ = 0;
    uint32_t consumeFailures_ = 0;
    uint32_t framesPopulated_ = 0;
    std::vector<uint8_t> frameBlobs_;
    std::vector<FrameColumn> columns_;

    // Bound PresentMonAPI2.dll functions (signature present in PresentMonAPI.h).
    PM_STATUS (*pmOpenSession_)(PM_SESSION_HANDLE*) = nullptr;
    PM_STATUS (*pmCloseSession_)(PM_SESSION_HANDLE) = nullptr;
    PM_STATUS (*pmGetApiVersion_)(PM_VERSION*) = nullptr;
    PM_STATUS (*pmStartTrackingProcess_)(PM_SESSION_HANDLE, uint32_t) = nullptr;
    PM_STATUS (*pmStopTrackingProcess_)(PM_SESSION_HANDLE, uint32_t) = nullptr;
    PM_STATUS (*pmSetTelemetryPollingPeriod_)(PM_SESSION_HANDLE, uint32_t, uint32_t) = nullptr;
    PM_STATUS (*pmGetIntrospectionRoot_)(PM_SESSION_HANDLE, const PM_INTROSPECTION_ROOT**) = nullptr;
    PM_STATUS (*pmFreeIntrospectionRoot_)(const PM_INTROSPECTION_ROOT*) = nullptr;
    PM_STATUS (*pmRegisterFrameQuery_)(PM_SESSION_HANDLE, PM_FRAME_QUERY_HANDLE*, PM_QUERY_ELEMENT*, uint64_t, uint32_t*) = nullptr;
    PM_STATUS (*pmFreeFrameQuery_)(PM_FRAME_QUERY_HANDLE) = nullptr;
    PM_STATUS (*pmConsumeFrames_)(PM_FRAME_QUERY_HANDLE, uint32_t, uint8_t*, uint32_t*) = nullptr;
};

// Translate enum metrics to their CSV string form.
char const* TranslatePresentMode(int v);
char const* TranslateRuntime(int v);
char const* TranslateFrameType(int v);
