#include "PmClient.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <string_view>

namespace {

void PrintWarning(char const* format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

void PrintError(char const* format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

constexpr uint32_t MaxFramesPerConsume = 256;

// The canonical CSV layout, column-for-column identical to the GUI capture
// application's CSV.  Columns unavailable on this machine are either dropped
// (trimmed output, the default) or written as NA (-all_columns), matching how
// the GUI fills its fixed layout.  Fan speed is the [0..3] array the GUI
// writes.  The hidden CPU_START_QPC element feeds the computed time columns.
FrameColumn const kGuiColumns[] = {
    { PM_METRIC_APPLICATION,                 0, "Application",                   false, true  },
    { PM_METRIC_PROCESS_ID,                  0, "ProcessID",                     false, true  },
    { PM_METRIC_SWAP_CHAIN_ADDRESS,          0, "SwapChainAddress",              false },
    { PM_METRIC_PRESENT_RUNTIME,             0, "PresentRuntime",                false },
    { PM_METRIC_SYNC_INTERVAL,               0, "SyncInterval",                  false },
    { PM_METRIC_PRESENT_FLAGS,               0, "PresentFlags",                  false },
    { PM_METRIC_ALLOWS_TEARING,              0, "AllowsTearing",                 false },
    { PM_METRIC_PRESENT_MODE,                0, "PresentMode",                   false },
    { PM_METRIC_FRAME_TYPE,                  0, "FrameType",                     false },
    { PM_METRIC_CPU_START_TIME,              0, "TimeInSeconds",                 false, true  },
    { PM_METRIC_BETWEEN_SIMULATION_START,    0, "MsBetweenSimulationStart",      false },
    { PM_METRIC_BETWEEN_PRESENTS,            0, "MsBetweenPresents",             false },
    { PM_METRIC_BETWEEN_DISPLAY_CHANGE,      0, "MsBetweenDisplayChange",        false },
    { PM_METRIC_IN_PRESENT_API,              0, "MsInPresentAPI",                false },
    { PM_METRIC_RENDER_PRESENT_LATENCY,      0, "MsRenderPresentLatency",        false },
    { PM_METRIC_UNTIL_DISPLAYED,             0, "MsUntilDisplayed",              false },
    { PM_METRIC_CPU_START_TIME,              0, "CPUStartTime",                  false, true  },
    { PM_METRIC_BETWEEN_APP_START,           0, "MsBetweenAppStart",             false },
    { PM_METRIC_CPU_BUSY,                    0, "MsCPUBusy",                     false },
    { PM_METRIC_CPU_WAIT,                    0, "MsCPUWait",                     false },
    { PM_METRIC_GPU_LATENCY,                 0, "MsGPULatency",                  false },
    { PM_METRIC_GPU_TIME,                    0, "MsGPUTime",                     false },
    { PM_METRIC_GPU_BUSY,                    0, "MsGPUBusy",                     false },
    { PM_METRIC_GPU_WAIT,                    0, "MsGPUWait",                     false },
    { PM_METRIC_PSO_COMPILE_COUNT,           0, "PSOCompileCount",               false },
    { PM_METRIC_PSO_COMPILE_TIME,            0, "MsPSOCompileTime",              false },
    { PM_METRIC_PSO_COMPILE_BUSY_PERCENT,    0, "PSOCompileBusyPercent",         false },
    { PM_METRIC_ANIMATION_ERROR,             0, "MsAnimationError",              false },
    { PM_METRIC_FLIP_DELAY,                  0, "MsFlipDelay",                   false },
    { PM_METRIC_ANIMATION_TIME,              0, "AnimationTime",                 false },
    { PM_METRIC_ALL_INPUT_TO_PHOTON_LATENCY, 0, "MsAllInputToPhotonLatency",     false },
    { PM_METRIC_CLICK_TO_PHOTON_LATENCY,     0, "MsClickToPhotonLatency",        false },
    { PM_METRIC_INSTRUMENTED_LATENCY,        0, "InstrumentedLatency",           false },
    { PM_METRIC_PC_LATENCY,                  0, "MsPCLatency",                   false },
    { PM_METRIC_GPU_POWER,                   0, "GPUPower",                      true  },
    { PM_METRIC_GPU_VOLTAGE,                 0, "GPUVoltage",                    true  },
    { PM_METRIC_GPU_FREQUENCY,               0, "GPUFrequency",                  true  },
    { PM_METRIC_GPU_TEMPERATURE,             0, "GPUTemperature",                true  },
    { PM_METRIC_GPU_UTILIZATION,             0, "GPUUtilization",                true  },
    { PM_METRIC_GPU_RENDER_COMPUTE_UTILIZATION, 0, "3D/ComputeUtilization",      true  },
    { PM_METRIC_GPU_MEDIA_UTILIZATION,       0, "MediaUtilization",              true  },
    { PM_METRIC_GPU_MEM_POWER,               0, "GPUMemoryPower",                true  },
    { PM_METRIC_GPU_MEM_VOLTAGE,             0, "GPUMemoryVoltage",              true  },
    { PM_METRIC_GPU_MEM_FREQUENCY,           0, "GPUMemoryFrequency",            true  },
    { PM_METRIC_GPU_MEM_EFFECTIVE_FREQUENCY, 0, "GPUMemoryEffectiveFrequency",   true  },
    { PM_METRIC_GPU_MEM_TEMPERATURE,         0, "GPUMemoryTemperature",          true  },
    { PM_METRIC_GPU_MEM_SIZE,                0, "GPUMemorySize",                 true  },
    { PM_METRIC_GPU_MEM_USED,                0, "GPUMemorySizeUsed",             true  },
    { PM_METRIC_GPU_MEM_MAX_BANDWIDTH,       0, "GPUMemoryMaxBandwidth",         true  },
    { PM_METRIC_GPU_MEM_READ_BANDWIDTH,      0, "GPUMemoryReadBandwidth",        true  },
    { PM_METRIC_GPU_MEM_WRITE_BANDWIDTH,     0, "GPUMemoryWriteBandwidth",       true  },
    { PM_METRIC_GPU_FAN_SPEED,               0, "GPUFanSpeed[0]",                true  },
    { PM_METRIC_GPU_FAN_SPEED,               1, "GPUFanSpeed[1]",                true  },
    { PM_METRIC_GPU_FAN_SPEED,               2, "GPUFanSpeed[2]",                true  },
    { PM_METRIC_GPU_FAN_SPEED,               3, "GPUFanSpeed[3]",                true  },
    { PM_METRIC_GPU_POWER_LIMITED,           0, "GPUPowerLimited",               true  },
    { PM_METRIC_GPU_TEMPERATURE_LIMITED,     0, "GPUTemperatureLimited",         true  },
    { PM_METRIC_GPU_CURRENT_LIMITED,         0, "GPUCurrentLimited",             true  },
    { PM_METRIC_GPU_VOLTAGE_LIMITED,         0, "GPUVoltageLimited",             true  },
    { PM_METRIC_GPU_UTILIZATION_LIMITED,     0, "GPUUtilizationLimited",         true  },
    { PM_METRIC_GPU_MEM_POWER_LIMITED,       0, "GPUMemoryPowerLimited",         true  },
    { PM_METRIC_GPU_MEM_TEMPERATURE_LIMITED, 0, "GPUMemoryTemperatureLimited",   true  },
    { PM_METRIC_GPU_MEM_CURRENT_LIMITED,     0, "GPUMemoryCurrentLimited",       true  },
    { PM_METRIC_GPU_MEM_VOLTAGE_LIMITED,     0, "GPUMemoryVoltageLimited",       true  },
    { PM_METRIC_GPU_MEM_UTILIZATION_LIMITED, 0, "GPUMemoryUtilizationLimited",   true  },
    { PM_METRIC_CPU_UTILIZATION,             0, "CPUUtilization",                false },
    { PM_METRIC_CPU_POWER,                   0, "CPUPower",                      false },
    { PM_METRIC_CPU_TEMPERATURE,             0, "CPUTemperature",                false },
    { PM_METRIC_CPU_FREQUENCY,               0, "CPUFrequency",                  false },
    // Hidden element: raw CPU start QPC feeding the computed time columns.
    { PM_METRIC_CPU_START_QPC,               0, "",                              false },
};

}

PmClient::~PmClient()
{
    Shutdown();
}

void PmClient::SetKeepAllColumns(bool keep)
{
    keepAllColumns_ = keep;
}

bool PmClient::BindFunctions()
{
    dll_ = LoadLibraryA("PresentMonAPI2.dll");
    if (dll_ == nullptr) {
        PrintError("error: failed to load PresentMonAPI2.dll (place it next to TelemetryMon.exe).\n");
        return false;
    }

    #define RESOLVE(name) \
        *reinterpret_cast<void**>(&name##_) = reinterpret_cast<void*>(GetProcAddress(dll_, #name)); \
        if (name##_ == nullptr) { \
            PrintError("error: PresentMonAPI2.dll is missing export %s.\n", #name); \
            return false; \
        }
    RESOLVE(pmOpenSession)
    RESOLVE(pmCloseSession)
    RESOLVE(pmGetApiVersion)
    RESOLVE(pmStartTrackingProcess)
    RESOLVE(pmStopTrackingProcess)
    RESOLVE(pmSetTelemetryPollingPeriod)
    RESOLVE(pmGetIntrospectionRoot)
    RESOLVE(pmFreeIntrospectionRoot)
    RESOLVE(pmRegisterFrameQuery)
    RESOLVE(pmFreeFrameQuery)
    RESOLVE(pmConsumeFrames)
    #undef RESOLVE
    return true;
}

bool PmClient::Init()
{
    if (!BindFunctions()) {
        return false;
    }

    PM_VERSION version = {};
    if (pmGetApiVersion_(&version) == PM_STATUS_SUCCESS) {
        printf("PresentMon API version: %u.%u.%u\n", version.major, version.minor, version.patch);
        fflush(stdout);
    }

    auto status = pmOpenSession_(&session_);
    if (status != PM_STATUS_SUCCESS) {
        PrintError("error: failed to open a session to the PresentMon service (0x%X).\n"
                   "       Is the 'PresentMonSharedService' Windows service running?\n", status);
        session_ = nullptr;
        return false;
    }
    return true;
}

void PmClient::Shutdown()
{
    if (session_ != nullptr) {
        if (pid_ != 0) {
            pmStopTrackingProcess_(session_, pid_);
        }
        if (frameQuery_ != nullptr) {
            pmFreeFrameQuery_(frameQuery_);
            frameQuery_ = nullptr;
        }
        pmCloseSession_(session_);
        session_ = nullptr;
        pid_ = 0;
    }
    if (dll_ != nullptr) {
        FreeLibrary(dll_);
        dll_ = nullptr;
    }
}

bool PmClient::Introspect()
{
    // Walk the introspection tree once: discover the active GPU device, then
    // resolve every layout column's availability.  All values are copied out
    // before the tree is freed (the tree owns the memory the pointers point
    // into).
    const PM_INTROSPECTION_ROOT* root = nullptr;
    if (pmGetIntrospectionRoot_(session_, &root) != PM_STATUS_SUCCESS || root == nullptr) {
        PrintError("error: failed to get introspection data from the service.\n");
        return false;
    }

    auto const* devicePtrs = (PM_INTROSPECTION_DEVICE const* const*)root->pDevices->pData;
    auto const* metricPtrs = (PM_INTROSPECTION_METRIC const* const*)root->pMetrics->pData;

    // GPU device: the device on which GPU power is available.
    for (size_t i = 0; i < root->pMetrics->size && gpuDeviceId_ == 0; ++i) {
        auto const* m = metricPtrs[i];
        if (m->id != PM_METRIC_GPU_POWER || m->pDeviceMetricInfo == nullptr) {
            continue;
        }
        auto const* infoPtrs = (PM_INTROSPECTION_DEVICE_METRIC_INFO const* const*)m->pDeviceMetricInfo->pData;
        for (size_t j = 0; j < m->pDeviceMetricInfo->size; ++j) {
            if (infoPtrs[j]->availability == PM_METRIC_AVAILABILITY_AVAILABLE) {
                gpuDeviceId_ = infoPtrs[j]->deviceId;
                break;
            }
        }
    }
    for (size_t i = 0; i < root->pDevices->size; ++i) {
        if (devicePtrs[i]->id == gpuDeviceId_ && devicePtrs[i]->pName && devicePtrs[i]->pName->pData) {
            for (char const* c = devicePtrs[i]->pName->pData; *c; ++c) {
                gpuName_.push_back(wchar_t(*c));
            }
        }
    }

    // Resolve availability of every layout column.
    columns_.assign(std::begin(kGuiColumns), std::end(kGuiColumns));
    for (auto& col : columns_) {
        if (col.computed) {
            col.available = true;
            continue;
        }

        PM_INTROSPECTION_METRIC const* m = nullptr;
        for (size_t i = 0; i < root->pMetrics->size; ++i) {
            if (metricPtrs[i]->id == col.metric) {
                m = metricPtrs[i];
                break;
            }
        }
        if (m == nullptr || m->pTypeInfo == nullptr) {
            continue;
        }
        if (m->type != PM_METRIC_TYPE_FRAME_EVENT && m->type != PM_METRIC_TYPE_DYNAMIC_FRAME) {
            continue;
        }
        if (m->pTypeInfo->frameType == PM_DATA_TYPE_VOID) {
            continue;
        }
        if (m->pDeviceMetricInfo == nullptr) {
            continue;
        }

        auto const* infoPtrs = (PM_INTROSPECTION_DEVICE_METRIC_INFO const* const*)m->pDeviceMetricInfo->pData;
        for (size_t j = 0; j < m->pDeviceMetricInfo->size; ++j) {
            if (infoPtrs[j]->availability != PM_METRIC_AVAILABILITY_AVAILABLE) {
                continue;
            }
            // GPU-scoped columns must land on the active GPU; for others take
            // the first available device (typically the system, 0).
            if (col.gpuScoped && infoPtrs[j]->deviceId != gpuDeviceId_) {
                continue;
            }
            // Array columns need the requested index to exist.
            if (col.arrayIndex >= infoPtrs[j]->arraySize) {
                continue;
            }
            col.deviceId = infoPtrs[j]->deviceId;
            col.type = m->pTypeInfo->frameType;
            col.available = true;
            break;
        }
    }

    pmFreeIntrospectionRoot_(root);

    // Default output trims the layout to the available columns: the three
    // computed columns go first, followed by every available query column
    // (the hidden QPC element becomes the visible CPUStartQPC column, as in
    // the classic console CSV).  With -all_columns the full GUI layout is
    // kept and unavailable columns are written as NA.
    if (!keepAllColumns_) {
        std::vector<FrameColumn> trimmed;
        for (auto& col : columns_) {
            if (!col.computed) {
                continue;
            }
            if (col.metric == PM_METRIC_APPLICATION || col.metric == PM_METRIC_PROCESS_ID) {
                trimmed.push_back(col);
            } else if (col.metric == PM_METRIC_CPU_START_TIME &&
                       std::string_view(col.header) == "TimeInSeconds") {
                trimmed.push_back(col);
            }
            // the CPUStartTime computed column is dropped (redundant here)
        }
        for (auto& col : columns_) {
            if (col.computed || !col.available) {
                continue;
            }
            if (col.metric == PM_METRIC_CPU_START_QPC) {
                col.header = "CPUStartQPC";
            }
            trimmed.push_back(col);
        }
        columns_.swap(trimmed);
    }

    if (gpuDeviceId_ == 0) {
        PrintWarning("warning: no GPU telemetry device found via introspection; GPU columns will be NA.\n");
    } else {
        printf("GPU telemetry device: id=%u %ls\n", gpuDeviceId_, gpuName_.c_str());
    }
    size_t availableCount = 0;
    for (auto const& col : columns_) {
        if (col.available) availableCount += 1;
    }
    printf("Columns: %zu (available: %zu)%s\n", columns_.size(), availableCount,
           keepAllColumns_ ? " [full GUI layout, NA for unavailable]" : " [trimmed to available]");
    fflush(stdout);
    return true;
}

bool PmClient::TrackProcess(uint32_t pid)
{
    auto status = pmStartTrackingProcess_(session_, pid);
    if (status != PM_STATUS_SUCCESS) {
        PrintError("error: failed to start tracking process %u (0x%X).\n", pid, status);
        return false;
    }
    pid_ = pid;
    return true;
}

bool PmClient::RegisterFrameQuery()
{
    std::vector<PM_QUERY_ELEMENT> elements;
    std::vector<size_t> elementColumnIndex;
    for (size_t i = 0; i < columns_.size(); ++i) {
        auto const& col = columns_[i];
        if (col.computed || !col.available || col.header[0] == '\0') {
            continue;
        }
        elements.push_back(PM_QUERY_ELEMENT{ col.metric, PM_STAT_NONE, col.deviceId, col.arrayIndex, 0, 0 });
        elementColumnIndex.push_back(i);
    }

    auto status = pmRegisterFrameQuery_(session_, &frameQuery_, elements.data(), elements.size(), &frameBlobSize_);
    if (status != PM_STATUS_SUCCESS) {
        PrintError("error: failed to register frame query (0x%X).\n", status);
        return false;
    }

    // Copy the service-assigned blob offsets/types back into the columns.
    for (size_t e = 0; e < elementColumnIndex.size(); ++e) {
        auto& col = columns_[elementColumnIndex[e]];
        col.offset = elements[e].dataOffset;
        col.size = elements[e].dataSize;
    }

    frameBlobs_.resize(size_t(frameBlobSize_) * MaxFramesPerConsume);
    return true;
}

uint32_t PmClient::ConsumeFrames()
{
    uint32_t n = MaxFramesPerConsume;
    auto status = pmConsumeFrames_(frameQuery_, pid_, frameBlobs_.data(), &n);
    if (status != PM_STATUS_SUCCESS) {
        if (consumeFailures_++ < 3) {
            fprintf(stderr, "pmConsumeFrames failed: 0x%X (n=%u)\n", status, n);
        }
        return 0;
    }
    framesPopulated_ = n;
    return n;
}

char const* TranslatePresentMode(int v)
{
    switch (v) {
    case PM_PRESENT_MODE_HARDWARE_LEGACY_FLIP:              return "Hardware: Legacy Flip";
    case PM_PRESENT_MODE_HARDWARE_LEGACY_COPY_TO_FRONT_BUFFER: return "Hardware: Legacy Copy to front buffer";
    case PM_PRESENT_MODE_HARDWARE_INDEPENDENT_FLIP:         return "Hardware: Independent Flip";
    case PM_PRESENT_MODE_COMPOSED_FLIP:                     return "Composed: Flip";
    case PM_PRESENT_MODE_COMPOSED_COPY_WITH_GPU_GDI:        return "Composed: Copy with GPU GDI";
    case PM_PRESENT_MODE_COMPOSED_COPY_WITH_CPU_GDI:        return "Composed: Copy with CPU GDI";
    case PM_PRESENT_MODE_HARDWARE_COMPOSED_INDEPENDENT_FLIP: return "Hardware Composed: Independent Flip";
    default:                                                return "Unknown";
    }
}

char const* TranslateRuntime(int v)
{
    switch (v) {
    case PM_GRAPHICS_RUNTIME_DXGI: return "DXGI";
    case PM_GRAPHICS_RUNTIME_D3D9: return "D3D9";
    default:                       return "Unknown";
    }
}

char const* TranslateFrameType(int v)
{
    switch (v) {
    case PM_FRAME_TYPE_UNSPECIFIED:  return "Unspecified";
    case PM_FRAME_TYPE_APPLICATION:  return "Application";
    case PM_FRAME_TYPE_REPEATED:     return "Repeated";
    case PM_FRAME_TYPE_INTEL_XEFG:   return "Intel XeSS-FG";
    case PM_FRAME_TYPE_AMD_AFMF:     return "AMD AFMF";
    default:                         return "NotSet";
    }
}
