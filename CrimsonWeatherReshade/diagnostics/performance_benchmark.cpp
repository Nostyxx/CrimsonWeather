#include "pch.h"

#if defined(CW_DEV_BUILD)

#include "performance_benchmark.h"
#include "runtime_shared.h"
#include "sky_texture_override.h"

#include <Psapi.h>
#include <TlHelp32.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

#pragma comment(lib, "Psapi.lib")

namespace {

constexpr double kCountdownSeconds = 5.0;
constexpr double kDefaultSettleSeconds = 1.5;
constexpr double kDefaultMeasureSeconds = 4.0;
constexpr double kTelemetryIntervalSeconds = 1.0;

enum class BenchmarkPhase {
    Idle,
    Countdown,
    Settling,
    Measuring,
    Complete,
    Aborted,
};

struct SliderSpec {
    const char* id;
    const char* group;
    SliderOverride* slot;
    float low;
    float mid;
    float high;
};

struct ColorSpec {
    const char* id;
    const char* group;
    ColorOverride* slot;
};

struct BoolSpec {
    const char* id;
    const char* group;
    std::atomic<bool>* value;
};

const std::vector<SliderSpec>& SliderSpecs() {
    static const std::vector<SliderSpec> specs = {
        { "rain", "weather", &g_oRain, 0.25f, 1.0f, 2.0f },
        { "thunder", "weather", &g_oThunder, 0.20f, 0.60f, 1.0f },
        { "snow", "weather", &g_oSnow, 0.25f, 1.0f, 2.0f },
        { "dust", "weather", &g_oDust, 0.25f, 1.0f, 2.0f },
        { "snow_accum_boundary_a", "weather", &g_oSnowAccumBoundaryA, -5.0f, -20.0f, -100.0f },
        { "snow_accum_boundary_b", "weather", &g_oSnowAccumBoundaryB, -20.0f, -100.0f, -500.0f },
        { "snow_coverage_threshold", "weather", &g_oSnowCoverageThreshold, -20.0f, -100.0f, -500.0f },

        { "cloud_amount", "atmosphere", &g_oCloudAmount, 0.50f, 2.0f, 8.0f },
        { "cloud_height", "atmosphere", &g_oCloudSpdX, -5.0f, 2.0f, 10.0f },
        { "cloud_density", "atmosphere", &g_oCloudSpdY, 0.50f, 2.0f, 8.0f },
        { "mid_clouds", "atmosphere", &g_oHighClouds, 0.50f, 2.0f, 8.0f },
        { "high_clouds", "atmosphere", &g_oAtmoAlpha, 0.50f, 2.0f, 8.0f },
        { "cloud_alpha", "atmosphere", &g_oCloudAlpha, 0.50f, 2.0f, 10.0f },
        { "cloud_fade_range", "atmosphere", &g_oCloudFadeRange, 1000.0f, 30000.0f, 100000.0f },
        { "cloud_detail_ratio", "atmosphere", &g_oCloudDetailRatio, 0.10f, 0.75f, 1.50f },
        { "cloud_phase_front", "atmosphere", &g_oCloudPhaseFront, -0.75f, 0.0f, 0.75f },
        { "cloud_scattering", "atmosphere", &g_oCloudScatteringCoefficient, 0.01f, 0.10f, 1.0f },
        { "cloud_flow", "atmosphere", &g_oCloudFlow, 0.25f, 2.0f, 8.0f },
        { "cloud_visible_range", "atmosphere", &g_oCloudVisibleRange, 0.25f, 2.0f, 8.0f },
        { "rayleigh_height", "atmosphere", &g_oRayleighHeight, 100.0f, 1200.0f, 10000.0f },
        { "ozone_ratio", "atmosphere", &g_oOzoneRatio, 0.10f, 1.0f, 8.0f },
        { "fog", "atmosphere", &g_oNativeFog, 0.25f, 2.0f, 8.0f },
        { "aerosol_height", "atmosphere", &g_oMieScaleHeight, 100.0f, 1200.0f, 10000.0f },
        { "aerosol_density", "atmosphere", &g_oMieAerosolDensity, 0.10f, 2.0f, 10.0f },
        { "aerosol_absorption", "atmosphere", &g_oMieAerosolAbsorption, 0.10f, 1.0f, 5.0f },
        { "fog_height_baseline", "atmosphere", &g_oHeightFogBaseline, -1000.0f, 0.0f, 1000.0f },
        { "fog_height_falloff", "atmosphere", &g_oHeightFogFalloff, 0.10f, 1.0f, 5.0f },

        { "night_sky_tilt", "celestial", &g_oExpNightSkyRot, -45.0f, 20.0f, 75.0f },
        { "night_sky_phase", "celestial", &g_oNightSkyYaw, -120.0f, 45.0f, 120.0f },
        { "sun_size", "celestial", &g_oSunSize, 0.10f, 1.0f, 5.0f },
        { "sun_light_intensity", "celestial", &g_oSunLightIntensity, 0.25f, 2.0f, 10.0f },
        { "sun_yaw", "celestial", &g_oSunDirX, -120.0f, 45.0f, 120.0f },
        { "sun_pitch", "celestial", &g_oSunDirY, -60.0f, 20.0f, 60.0f },
        { "moon_size", "celestial", &g_oMoonSize, 0.10f, 2.0f, 10.0f },
        { "moon_light_intensity", "celestial", &g_oMoonLightIntensity, 0.25f, 2.0f, 10.0f },
        { "moon_yaw", "celestial", &g_oMoonDirX, -120.0f, 45.0f, 120.0f },
        { "moon_pitch", "celestial", &g_oMoonDirY, -60.0f, 20.0f, 60.0f },
        { "moon_rotation", "celestial", &g_oMoonRoll, -120.0f, 45.0f, 120.0f },

        { "experiment_2c", "experiment", &g_oExpCloud2C, 0.50f, 2.0f, 8.0f },
        { "experiment_2d", "experiment", &g_oExpCloud2D, 0.50f, 2.0f, 8.0f },
        { "cloud_variation", "experiment", &g_oCloudVariation, 0.50f, 2.0f, 8.0f },
        { "puddle_scale", "experiment", &g_oCloudThk, 0.10f, 0.50f, 1.0f },
    };
    return specs;
}

const std::vector<ColorSpec>& ColorSpecs() {
    static const std::vector<ColorSpec> specs = {
        { "rayleigh_color", "atmosphere", &g_oRayleighScatteringColor },
        { "volume_fog_color", "atmosphere", &g_oVolumeFogScatterColor },
        { "mie_scatter_color", "atmosphere", &g_oMieScatterColor },
    };
    return specs;
}

const std::vector<BoolSpec>& BoolSpecs() {
    static const std::vector<BoolSpec> specs = {
        { "force_clear", "weather", &g_forceClear },
        { "no_rain", "weather", &g_noRain },
        { "no_dust", "weather", &g_noDust },
        { "no_snow", "weather", &g_noSnow },
        { "no_wind", "general", &g_noWind },
        { "no_fog", "atmosphere", &g_noFog },
    };
    return specs;
}

struct SliderSnapshot {
    bool active = false;
    float value = 0.0f;
};

struct ColorSnapshot {
    bool active = false;
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct RuntimeSnapshot {
    std::vector<SliderSnapshot> sliders;
    std::vector<ColorSnapshot> colors;
    std::vector<bool> booleans;
    bool modEnabled = true;
    float wind = 1.0f;
    SliderSnapshot legacyWind{};
    SliderSnapshot actualWind{};
    bool timeCtrlActive = false;
    bool timeFreeze = false;
    bool timeProgress = false;
    bool timeMatch = false;
    float timeCadence = 0.0f;
    float timeTarget = 12.0f;
    bool realTimeEnabled = false;
    float dayScale = 1.0f;
    float nightScale = 1.0f;
    std::string moonTexture;
    std::string milkywayTexture;
};

struct ResourceSample {
    uint64_t processKernel100ns = 0;
    uint64_t processUser100ns = 0;
    uint64_t workingSet = 0;
    uint64_t privateBytes = 0;
    uint64_t pageFaults = 0;
    uint32_t handles = 0;
    uint32_t threads = 0;
    uint64_t gpuLocalUsage = 0;
    uint64_t gpuLocalBudget = 0;
    uint64_t gpuNonLocalUsage = 0;
    uint64_t gpuNonLocalBudget = 0;
};

struct TelemetryPoint {
    double elapsedSeconds = 0.0;
    ResourceSample resource{};
};

struct Scenario {
    std::string id;
    std::string group;
    std::string state;
    double settleSeconds = kDefaultSettleSeconds;
    double measureSeconds = kDefaultMeasureSeconds;
    std::function<void()> apply;
    std::function<void(double)> tick;
};

RuntimeSnapshot g_snapshot{};
std::vector<Scenario> g_scenarios;
size_t g_scenarioIndex = 0;
BenchmarkPhase g_phase = BenchmarkPhase::Idle;
LARGE_INTEGER g_qpcFrequency{};
int64_t g_phaseStartedQpc = 0;
int64_t g_runStartedQpc = 0;
int64_t g_lastFrameQpc = 0;
int64_t g_lastTelemetryQpc = 0;
double g_totalPlannedSeconds = 0.0;
double g_completedPlannedSeconds = 0.0;
std::vector<double> g_frameTimesMs;
std::vector<TelemetryPoint> g_telemetry;
size_t g_profilerIntervalsSkipped = 0;
ResourceSample g_resourceStart{};
FILE* g_report = nullptr;
IDXGIAdapter3* g_adapter = nullptr;
DXGI_ADAPTER_DESC1 g_adapterDesc{};
std::atomic<bool> g_abortRequested{ false };
char g_reportPath[MAX_PATH * 2] = {};
char g_lastReportPath[MAX_PATH * 2] = {};
char g_message[256] = "Ready";

double QpcSeconds(int64_t delta) {
    return g_qpcFrequency.QuadPart > 0
        ? static_cast<double>(delta) / static_cast<double>(g_qpcFrequency.QuadPart)
        : 0.0;
}

int64_t QpcNow() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

uint64_t FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER converted{};
    converted.LowPart = value.dwLowDateTime;
    converted.HighPart = value.dwHighDateTime;
    return converted.QuadPart;
}

uint32_t ProcessThreadCount() {
    const DWORD processId = GetCurrentProcessId();
    uint32_t count = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == processId) {
                ++count;
            }
        } while (Thread32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}

void ResolveAdapter(ID3D12Device* device) {
    if (g_adapter || !device) {
        return;
    }

    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) {
        return;
    }

    const LUID wanted = device->GetAdapterLuid();
    for (UINT index = 0;; ++index) {
        IDXGIAdapter1* candidate = nullptr;
        if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (!candidate) {
            continue;
        }
        DXGI_ADAPTER_DESC1 desc{};
        candidate->GetDesc1(&desc);
        if (desc.AdapterLuid.HighPart == wanted.HighPart && desc.AdapterLuid.LowPart == wanted.LowPart) {
            if (SUCCEEDED(candidate->QueryInterface(IID_PPV_ARGS(&g_adapter)))) {
                g_adapterDesc = desc;
            }
            candidate->Release();
            break;
        }
        candidate->Release();
    }
    factory->Release();
}

ResourceSample CaptureResourceSample() {
    ResourceSample sample{};
    HANDLE process = GetCurrentProcess();

    FILETIME created{}, exited{}, kernel{}, user{};
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        sample.processKernel100ns = FileTimeValue(kernel);
        sample.processUser100ns = FileTimeValue(user);
    }

    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory))) {
        sample.workingSet = memory.WorkingSetSize;
        sample.privateBytes = memory.PrivateUsage;
        sample.pageFaults = memory.PageFaultCount;
    }
    GetProcessHandleCount(process, reinterpret_cast<PDWORD>(&sample.handles));
    sample.threads = ProcessThreadCount();

    if (g_adapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            sample.gpuLocalUsage = info.CurrentUsage;
            sample.gpuLocalBudget = info.Budget;
        }
        if (SUCCEEDED(g_adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) {
            sample.gpuNonLocalUsage = info.CurrentUsage;
            sample.gpuNonLocalBudget = info.Budget;
        }
    }
    return sample;
}

std::string JsonEscape(const char* text) {
    std::string out;
    if (!text) {
        return out;
    }
    for (const unsigned char c : std::string(text)) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c >= 0x20) {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

std::string WideToUtf8(const wchar_t* text) {
    if (!text || !text[0]) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    out.pop_back();
    return out;
}

void CaptureRuntimeSnapshot() {
    g_snapshot = RuntimeSnapshot{};
    for (const SliderSpec& spec : SliderSpecs()) {
        g_snapshot.sliders.push_back({ spec.slot->active.load(), spec.slot->value.load() });
    }
    for (const ColorSpec& spec : ColorSpecs()) {
        g_snapshot.colors.push_back({
            spec.slot->active.load(), spec.slot->r.load(), spec.slot->g.load(),
            spec.slot->b.load(), spec.slot->a.load()
        });
    }
    for (const BoolSpec& spec : BoolSpecs()) {
        g_snapshot.booleans.push_back(spec.value->load());
    }
    g_snapshot.modEnabled = g_modEnabled.load();
    g_snapshot.wind = g_windMul.load();
    g_snapshot.legacyWind = { g_oWind.active.load(), g_oWind.value.load() };
    g_snapshot.actualWind = { g_oWindActual.active.load(), g_oWindActual.value.load() };
    g_snapshot.timeCtrlActive = g_timeCtrlActive.load();
    g_snapshot.timeFreeze = g_timeFreeze.load();
    g_snapshot.timeProgress = g_timeProgressVisualTime.load();
    g_snapshot.timeMatch = g_timeProgressMatchGameTime.load();
    g_snapshot.timeCadence = g_timeProgressCadenceMs.load();
    g_snapshot.timeTarget = g_timeTargetHour.load();
    g_snapshot.realTimeEnabled = g_realGameTimeEnabled.load();
    g_snapshot.dayScale = g_realGameTimeDayScale.load();
    g_snapshot.nightScale = g_realGameTimeNightScale.load();

    const int moon = MoonTextureSelectedOption();
    const int milkyway = MilkywayTextureSelectedOption();
    if (moon >= 0 && moon < MoonTextureOptionCount()) {
        g_snapshot.moonTexture = MoonTextureOptionName(moon);
    }
    if (milkyway >= 0 && milkyway < MilkywayTextureOptionCount()) {
        g_snapshot.milkywayTexture = MilkywayTextureOptionName(milkyway);
    }
}

void SetAllInactive() {
    for (const SliderSpec& spec : SliderSpecs()) {
        spec.slot->clear();
    }
    for (const ColorSpec& spec : ColorSpecs()) {
        spec.slot->clear();
    }
    for (const BoolSpec& spec : BoolSpecs()) {
        spec.value->store(false);
    }
    g_oWind.clear();
    g_oWindActual.clear();
    g_snowCoverageGlobalsDirty.store(true);
    g_windMul.store(1.0f);
    g_modEnabled.store(true);
    g_timeCtrlActive.store(false);
    g_timeFreeze.store(false);
    g_timeProgressVisualTime.store(false);
    g_timeProgressMatchGameTime.store(false);
    g_timeProgressLastTick.store(0);
    g_timeProgressMatchLastMinute.store(-1);
    g_timeProgressMatchPendingMs.store(0);
    g_realGameTimeEnabled.store(false);
    g_realGameTimeSetMinuteRequest.store(-1);
    g_realGameTimeDayDeltaRequest.store(0);
    g_realGameTimeDayScale.store(1.0f);
    g_realGameTimeNightScale.store(1.0f);
    g_timeApplyRequest.store(true);
    g_resetStopRequested.store(true);
    if (MoonTextureOptionCount() > 0 && MoonTextureSelectedOption() != 0) {
        MoonTextureSelectOption(0);
    }
    if (MilkywayTextureOptionCount() > 0 && MilkywayTextureSelectedOption() != 0) {
        MilkywayTextureSelectOption(0);
    }
}

void RestoreRuntimeSnapshot() {
    for (size_t i = 0; i < SliderSpecs().size() && i < g_snapshot.sliders.size(); ++i) {
        const SliderSnapshot& saved = g_snapshot.sliders[i];
        if (saved.active) {
            SliderSpecs()[i].slot->set(saved.value);
        } else {
            SliderSpecs()[i].slot->clear();
        }
    }
    for (size_t i = 0; i < ColorSpecs().size() && i < g_snapshot.colors.size(); ++i) {
        const ColorSnapshot& saved = g_snapshot.colors[i];
        if (saved.active) {
            ColorSpecs()[i].slot->set(saved.r, saved.g, saved.b, saved.a);
        } else {
            ColorSpecs()[i].slot->clear();
        }
    }
    for (size_t i = 0; i < BoolSpecs().size() && i < g_snapshot.booleans.size(); ++i) {
        BoolSpecs()[i].value->store(g_snapshot.booleans[i]);
    }
    g_modEnabled.store(g_snapshot.modEnabled);
    g_windMul.store(g_snapshot.wind);
    if (g_snapshot.legacyWind.active) g_oWind.set(g_snapshot.legacyWind.value);
    else g_oWind.clear();
    if (g_snapshot.actualWind.active) g_oWindActual.set(g_snapshot.actualWind.value);
    else g_oWindActual.clear();
    g_snowCoverageGlobalsDirty.store(true);
    g_timeCtrlActive.store(g_snapshot.timeCtrlActive);
    g_timeFreeze.store(g_snapshot.timeFreeze);
    g_timeProgressVisualTime.store(g_snapshot.timeProgress);
    g_timeProgressMatchGameTime.store(g_snapshot.timeMatch);
    g_timeProgressCadenceMs.store(g_snapshot.timeCadence);
    g_timeTargetHour.store(g_snapshot.timeTarget);
    g_realGameTimeEnabled.store(g_snapshot.realTimeEnabled);
    g_realGameTimeDayScale.store(g_snapshot.dayScale);
    g_realGameTimeNightScale.store(g_snapshot.nightScale);
    g_timeProgressLastTick.store(g_snapshot.timeProgress ? GetTickCount64() : 0);
    g_timeApplyRequest.store(true);
    g_resetStopRequested.store(true);
    if (!g_snapshot.moonTexture.empty() && !MoonTextureSelectByName(g_snapshot.moonTexture.c_str())) {
        MoonTextureSelectOption(0);
    }
    if (!g_snapshot.milkywayTexture.empty() && !MilkywayTextureSelectByName(g_snapshot.milkywayTexture.c_str())) {
        MilkywayTextureSelectOption(0);
    }
}

void AddScenario(const char* id,
                 const char* group,
                 const char* state,
                 std::function<void()> apply = {},
                 double measureSeconds = kDefaultMeasureSeconds,
                 double settleSeconds = kDefaultSettleSeconds,
                 std::function<void(double)> tick = {}) {
    Scenario scenario{};
    scenario.id = id ? id : "unknown";
    scenario.group = group ? group : "unknown";
    scenario.state = state ? state : "";
    scenario.apply = std::move(apply);
    scenario.measureSeconds = measureSeconds;
    scenario.settleSeconds = settleSeconds;
    scenario.tick = std::move(tick);
    g_totalPlannedSeconds += scenario.settleSeconds + scenario.measureSeconds;
    g_scenarios.push_back(std::move(scenario));
}

void SetSliderValue(SliderOverride* slot, float value) {
    if (!slot) {
        return;
    }
    slot->set(value);
    if (slot == &g_oSnowAccumBoundaryA || slot == &g_oSnowAccumBoundaryB ||
        slot == &g_oSnowCoverageThreshold) {
        g_snowCoverageGlobalsDirty.store(true);
    }
}

void AddBaseline(const std::string& suffix) {
    AddScenario(("baseline." + suffix).c_str(), "baseline", "all_runtime_controls_inactive");
}

void SetGroupMidpoint(const char* group) {
    for (const SliderSpec& spec : SliderSpecs()) {
        if (_stricmp(spec.group, group) == 0) {
            SetSliderValue(spec.slot, spec.mid);
        }
    }
    for (const ColorSpec& spec : ColorSpecs()) {
        if (_stricmp(spec.group, group) == 0) {
            spec.slot->set(0.75f, 0.90f, 1.0f, 1.0f);
        }
    }
}

int FindTextureOption(bool moon, bool animated) {
    const int count = moon ? MoonTextureOptionCount() : MilkywayTextureOptionCount();
    for (int i = 1; i < count; ++i) {
        const bool optionAnimated = moon ? MoonTextureOptionIsAnimated(i) : MilkywayTextureOptionIsAnimated(i);
        const char* name = moon ? MoonTextureOptionName(i) : MilkywayTextureOptionName(i);
        const bool blankOption = name &&
            (_stricmp(name, "No Moon") == 0 || _stricmp(name, "No Milky Way") == 0);
        if (!blankOption && optionAnimated == animated) {
            return i;
        }
    }
    return -1;
}

void BuildScenarios() {
    g_scenarios.clear();
    g_totalPlannedSeconds = kCountdownSeconds;
    AddBaseline("initial");

    const char* valueNames[] = { "low", "mid", "high" };
    for (const SliderSpec& spec : SliderSpecs()) {
        const float values[] = { spec.low, spec.mid, spec.high };
        for (size_t valueIndex = 0; valueIndex < 3; ++valueIndex) {
            char id[128] = {};
            char state[96] = {};
            sprintf_s(id, "%s.%s", spec.id, valueNames[valueIndex]);
            sprintf_s(state, "active=1,value=%.6f", values[valueIndex]);
            SliderOverride* slot = spec.slot;
            const float value = values[valueIndex];
            AddScenario(id, spec.group, state, [slot, value]() { SetSliderValue(slot, value); });
            AddBaseline(std::string("after.") + id);
        }
    }

    for (const BoolSpec& spec : BoolSpecs()) {
        const std::string id = std::string(spec.id) + ".enabled";
        std::atomic<bool>* value = spec.value;
        AddScenario(id.c_str(), spec.group, "enabled=1", [value]() { value->store(true); });
        AddBaseline(std::string("after.") + id);
    }

    for (const ColorSpec& spec : ColorSpecs()) {
        ColorOverride* slot = spec.slot;
        const std::string neutralId = std::string(spec.id) + ".neutral";
        AddScenario(neutralId.c_str(), spec.group, "active=1,rgba=1,1,1,1",
            [slot]() { slot->set(1.0f, 1.0f, 1.0f, 1.0f); });
        AddBaseline(std::string("after.") + neutralId);
        const std::string tintId = std::string(spec.id) + ".tint";
        AddScenario(tintId.c_str(), spec.group, "active=1,rgba=0.25,0.75,1,1",
            [slot]() { slot->set(0.25f, 0.75f, 1.0f, 1.0f); });
        AddBaseline(std::string("after.") + tintId);
    }

    const float windValues[] = { 0.25f, 2.0f, 10.0f };
    for (size_t i = 0; i < 3; ++i) {
        char id[64] = {};
        char state[64] = {};
        sprintf_s(id, "wind.%s", valueNames[i]);
        sprintf_s(state, "active=1,value=%.3f", windValues[i]);
        const float value = windValues[i];
        AddScenario(id, "general", state, [value]() { g_windMul.store(value); });
        AddBaseline(std::string("after.") + id);
    }

    AddScenario("visual_time.fixed", "time", "mode=visual_fixed,hour=12",
        []() {
            g_timeTargetHour.store(12.0f);
            g_timeCtrlActive.store(true);
            g_timeFreeze.store(true);
            g_timeApplyRequest.store(true);
        });
    AddBaseline("after.visual_time.fixed");
    AddScenario("visual_time.progress", "time", "mode=visual_progress,cadence_ms=0",
        []() {
            g_timeTargetHour.store(12.0f);
            g_timeProgressCadenceMs.store(0.0f);
            g_timeProgressVisualTime.store(true);
            g_timeProgressLastTick.store(GetTickCount64());
            g_timeCtrlActive.store(true);
            g_timeFreeze.store(true);
            g_timeApplyRequest.store(true);
        });
    AddBaseline("after.visual_time.progress");
    for (const float scale : { 0.25f, 4.0f }) {
        char id[64] = {};
        char state[64] = {};
        sprintf_s(id, "real_time.scale_%.2f", scale);
        sprintf_s(state, "mode=real_time,day_scale=%.3f,night_scale=%.3f", scale, scale);
        AddScenario(id, "time", state, [scale]() {
            g_realGameTimeDayScale.store(scale);
            g_realGameTimeNightScale.store(scale);
            g_realGameTimeEnabled.store(true);
            g_timeApplyRequest.store(true);
        });
        AddBaseline(std::string("after.") + id);
    }

    MoonTextureRefreshList();
    MilkywayTextureRefreshList();
    for (const bool moon : { true, false }) {
        for (const bool animated : { false, true }) {
            const int option = FindTextureOption(moon, animated);
            if (option < 0) {
                continue;
            }
            const char* kind = moon ? "moon" : "milkyway";
            const char* mode = animated ? "animated" : "static";
            const char* optionName = moon ? MoonTextureOptionName(option) : MilkywayTextureOptionName(option);
            const std::string id = std::string("texture.") + kind + "." + mode;
            const std::string state = std::string("option=") + (optionName ? optionName : "unknown");
            AddScenario(id.c_str(), "texture", state.c_str(), [moon, option]() {
                if (moon) MoonTextureSelectOption(option);
                else MilkywayTextureSelectOption(option);
            }, 10.0, 0.0);
            AddBaseline(std::string("after.") + id);
        }
    }

    AddScenario("transition.rain_sweep", "transition", "rain_cycles_0_to_2",
        []() { g_oRain.set(0.0f); }, 10.0, 0.0,
        [](double elapsed) { g_oRain.set(static_cast<float>((sin(elapsed * 3.141592653589793) + 1.0) * 1.0)); });
    AddBaseline("after.transition.rain_sweep");
    AddScenario("transition.cloud_sweep", "transition", "cloud_amount_cycles_0.25_to_8",
        []() { g_oCloudAmount.set(0.25f); }, 10.0, 0.0,
        [](double elapsed) { g_oCloudAmount.set(0.25f + static_cast<float>((sin(elapsed * 2.0) + 1.0) * 3.875)); });
    AddBaseline("after.transition.cloud_sweep");
    AddScenario("transition.wind_sweep", "transition", "wind_cycles_0_to_10",
        []() { g_windMul.store(1.0f); }, 10.0, 0.0,
        [](double elapsed) { g_windMul.store(static_cast<float>((sin(elapsed * 2.0) + 1.0) * 5.0)); });
    AddBaseline("after.transition.wind_sweep");

    for (const char* group : { "weather", "atmosphere", "celestial", "experiment" }) {
        const std::string id = std::string("group.") + group;
        AddScenario(id.c_str(), group, "all_group_sliders_mid", [group]() { SetGroupMidpoint(group); }, 8.0);
        AddBaseline(std::string("after.") + id);
    }
    AddScenario("group.all", "stress", "all_sliders_mid",
        []() {
            SetGroupMidpoint("weather");
            SetGroupMidpoint("atmosphere");
            SetGroupMidpoint("celestial");
            SetGroupMidpoint("experiment");
            g_windMul.store(2.0f);
        }, 12.0, 2.0);
    AddBaseline("after.group.all");
    AddScenario("soak.all_mid", "soak", "all_sliders_mid,300_seconds",
        []() {
            SetGroupMidpoint("weather");
            SetGroupMidpoint("atmosphere");
            SetGroupMidpoint("celestial");
            SetGroupMidpoint("experiment");
            g_windMul.store(2.0f);
        }, 300.0, 3.0);
    AddBaseline("final");
}

double Percentile(const std::vector<double>& sorted, double percentile) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double index = percentile * static_cast<double>(sorted.size() - 1);
    const size_t lower = static_cast<size_t>(floor(index));
    const size_t upper = static_cast<size_t>(ceil(index));
    const double fraction = index - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

void WriteResourceJson(const ResourceSample& sample) {
    fprintf(g_report,
        "{\"cpuKernel100ns\":%llu,\"cpuUser100ns\":%llu,\"workingSet\":%llu,"
        "\"privateBytes\":%llu,\"pageFaults\":%llu,\"handles\":%u,\"threads\":%u,"
        "\"gpuLocalUsage\":%llu,\"gpuLocalBudget\":%llu,\"gpuNonLocalUsage\":%llu,"
        "\"gpuNonLocalBudget\":%llu}",
        static_cast<unsigned long long>(sample.processKernel100ns),
        static_cast<unsigned long long>(sample.processUser100ns),
        static_cast<unsigned long long>(sample.workingSet),
        static_cast<unsigned long long>(sample.privateBytes),
        static_cast<unsigned long long>(sample.pageFaults),
        sample.handles,
        sample.threads,
        static_cast<unsigned long long>(sample.gpuLocalUsage),
        static_cast<unsigned long long>(sample.gpuLocalBudget),
        static_cast<unsigned long long>(sample.gpuNonLocalUsage),
        static_cast<unsigned long long>(sample.gpuNonLocalBudget));
}

void WriteScenarioResult(const Scenario& scenario, const ResourceSample& resourceEnd, double measuredSeconds) {
    if (!g_report) {
        return;
    }

    std::vector<double> sorted = g_frameTimesMs;
    std::sort(sorted.begin(), sorted.end());
    const double totalMs = std::accumulate(g_frameTimesMs.begin(), g_frameTimesMs.end(), 0.0);
    const double meanMs = g_frameTimesMs.empty() ? 0.0 : totalMs / static_cast<double>(g_frameTimesMs.size());
    const double fps = meanMs > 0.0 ? 1000.0 / meanMs : 0.0;
    const uint64_t cpuDelta =
        (resourceEnd.processKernel100ns + resourceEnd.processUser100ns) -
        (g_resourceStart.processKernel100ns + g_resourceStart.processUser100ns);
    const double logicalProcessors = static_cast<double>(std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
    const double cpuPercent = measuredSeconds > 0.0
        ? (static_cast<double>(cpuDelta) / 10000000.0) / measuredSeconds * 100.0 / logicalProcessors
        : 0.0;

    size_t over16 = 0, over25 = 0, over33 = 0, over50 = 0, over100 = 0;
    size_t longestStutterRun = 0, currentStutterRun = 0;
    for (const double frame : g_frameTimesMs) {
        if (frame > 16.6667) ++over16;
        if (frame > 25.0) ++over25;
        if (frame > 33.3333) ++over33;
        if (frame > 50.0) ++over50;
        if (frame > 100.0) ++over100;
        if (frame > 33.3333) {
            longestStutterRun = max(longestStutterRun, ++currentStutterRun);
        } else {
            currentStutterRun = 0;
        }
    }

    fprintf(g_report,
        "{\"type\":\"scenario\",\"index\":%zu,\"id\":\"%s\",\"group\":\"%s\","
        "\"state\":\"%s\",\"settleSeconds\":%.3f,\"requestedMeasureSeconds\":%.3f,"
        "\"measuredSeconds\":%.6f,\"profilerIntervalsSkipped\":%zu,"
        "\"regionMajor\":%d,\"regionLocal\":%d,\"gameHour\":%.6f,"
        "\"frames\":%zu,\"fps\":%.6f,\"meanMs\":%.6f,\"p50Ms\":%.6f,"
        "\"p90Ms\":%.6f,\"p95Ms\":%.6f,\"p99Ms\":%.6f,\"p999Ms\":%.6f,"
        "\"maxMs\":%.6f,\"onePercentLowFps\":%.6f,\"pointOnePercentLowFps\":%.6f,"
        "\"over16_67Ms\":%zu,\"over25Ms\":%zu,\"over33_33Ms\":%zu,"
        "\"over50Ms\":%zu,\"over100Ms\":%zu,\"longestOver33Run\":%zu,"
        "\"normalizedProcessCpuPercent\":%.6f,\"resourceStart\":",
        g_scenarioIndex,
        JsonEscape(scenario.id.c_str()).c_str(),
        JsonEscape(scenario.group.c_str()).c_str(),
        JsonEscape(scenario.state.c_str()).c_str(),
        scenario.settleSeconds,
        scenario.measureSeconds,
        measuredSeconds,
        g_profilerIntervalsSkipped,
        g_regionMajorId.load(),
        g_regionLocalId.load(),
        g_timeCurrentHour.load(),
        g_frameTimesMs.size(),
        fps,
        meanMs,
        Percentile(sorted, 0.50),
        Percentile(sorted, 0.90),
        Percentile(sorted, 0.95),
        Percentile(sorted, 0.99),
        Percentile(sorted, 0.999),
        sorted.empty() ? 0.0 : sorted.back(),
        Percentile(sorted, 0.99) > 0.0 ? 1000.0 / Percentile(sorted, 0.99) : 0.0,
        Percentile(sorted, 0.999) > 0.0 ? 1000.0 / Percentile(sorted, 0.999) : 0.0,
        over16, over25, over33, over50, over100, longestStutterRun,
        cpuPercent);
    WriteResourceJson(g_resourceStart);
    fprintf(g_report, ",\"resourceEnd\":");
    WriteResourceJson(resourceEnd);
    fprintf(g_report, ",\"telemetry\":[");
    for (size_t i = 0; i < g_telemetry.size(); ++i) {
        if (i) fputc(',', g_report);
        fprintf(g_report, "{\"elapsedSeconds\":%.6f,\"resource\":", g_telemetry[i].elapsedSeconds);
        WriteResourceJson(g_telemetry[i].resource);
        fputc('}', g_report);
    }
    fprintf(g_report, "],\"frameTimesMs\":[");
    for (size_t i = 0; i < g_frameTimesMs.size(); ++i) {
        if (i) fputc(',', g_report);
        fprintf(g_report, "%.6f", g_frameTimesMs[i]);
    }
    fprintf(g_report, "]}\n");
    fflush(g_report);
}

void WriteRunHeader() {
    if (!g_report) {
        return;
    }
    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    char executable[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, executable, MAX_PATH);
    WIN32_FILE_ATTRIBUTE_DATA exeData{};
    GetFileAttributesExA(executable, GetFileExInfoStandard, &exeData);
    ULARGE_INTEGER exeSize{};
    exeSize.LowPart = exeData.nFileSizeLow;
    exeSize.HighPart = exeData.nFileSizeHigh;

    RuntimeHookStatusEntry hooks[static_cast<size_t>(RuntimeHookId::Count)]{};
    const size_t hookCount = GetRuntimeHookStatusEntries(hooks, std::size(hooks));
    fprintf(g_report,
        "{\"type\":\"run_start\",\"formatVersion\":1,\"modVersion\":\"%s\","
        "\"executable\":\"%s\",\"executableBytes\":%llu,\"logicalProcessors\":%u,"
        "\"scenarioCount\":%zu,\"plannedSeconds\":%.3f,\"gpu\":\"%s\",\"hooks\":[",
        MOD_VERSION,
        JsonEscape(executable).c_str(),
        static_cast<unsigned long long>(exeSize.QuadPart),
        systemInfo.dwNumberOfProcessors,
        g_scenarios.size(),
        g_totalPlannedSeconds,
        JsonEscape(WideToUtf8(g_adapterDesc.Description).c_str()).c_str());
    for (size_t i = 0; i < hookCount; ++i) {
        if (i) fputc(',', g_report);
        fprintf(g_report,
            "{\"name\":\"%s\",\"kind\":\"%s\",\"installed\":%s,\"enabled\":%s}",
            JsonEscape(hooks[i].name).c_str(), JsonEscape(hooks[i].kind).c_str(),
            hooks[i].installed ? "true" : "false", hooks[i].enabled ? "true" : "false");
    }
    fprintf(g_report, "]}\n");
    fflush(g_report);
}

bool OpenReport(char* outError, size_t outErrorSize) {
    std::filesystem::path directory = std::filesystem::path(g_pluginDir) / "CrimsonWeather" / "Diagnostics";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        if (outError && outErrorSize) sprintf_s(outError, outErrorSize, "Could not create Diagnostics directory");
        return false;
    }

    SYSTEMTIME time{};
    GetLocalTime(&time);
    char fileName[128] = {};
    sprintf_s(fileName, "CrimsonWeatherPerformance_%04u%02u%02u_%02u%02u%02u.jsonl",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    const std::filesystem::path path = directory / fileName;
    strncpy_s(g_reportPath, path.string().c_str(), _TRUNCATE);
    g_report = _fsopen(g_reportPath, "wb", _SH_DENYNO);
    if (!g_report) {
        if (outError && outErrorSize) sprintf_s(outError, outErrorSize, "Could not open performance report");
        g_reportPath[0] = '\0';
        return false;
    }
    setvbuf(g_report, nullptr, _IOFBF, 1024 * 1024);
    return true;
}

void EnterScenario(int64_t now) {
    if (g_scenarioIndex >= g_scenarios.size()) {
        return;
    }
    SetAllInactive();
    const Scenario& scenario = g_scenarios[g_scenarioIndex];
    if (scenario.apply) {
        scenario.apply();
    }
    g_phase = BenchmarkPhase::Settling;
    g_phaseStartedQpc = now;
    g_lastFrameQpc = 0;
    sprintf_s(g_message, "Settling: %s", scenario.id.c_str());
}

void BeginMeasurement(int64_t now) {
    g_phase = BenchmarkPhase::Measuring;
    g_phaseStartedQpc = now;
    g_lastFrameQpc = 0;
    g_lastTelemetryQpc = now;
    g_frameTimesMs.clear();
    g_telemetry.clear();
    g_profilerIntervalsSkipped = 0;
    g_frameTimesMs.reserve(static_cast<size_t>(max(512.0, g_scenarios[g_scenarioIndex].measureSeconds * 240.0)));
    g_resourceStart = CaptureResourceSample();
    sprintf_s(g_message, "Measuring: %s", g_scenarios[g_scenarioIndex].id.c_str());
}

void FinishRun(bool aborted, const char* reason) {
    RestoreRuntimeSnapshot();
    g_devPerformanceBenchmarkActive.store(false);
    if (g_report) {
        fprintf(g_report,
            "{\"type\":\"run_end\",\"status\":\"%s\",\"completedScenarios\":%zu,"
            "\"scenarioCount\":%zu,\"elapsedSeconds\":%.6f,\"reason\":\"%s\"}\n",
            aborted ? "aborted" : "complete",
            g_scenarioIndex,
            g_scenarios.size(),
            QpcSeconds(QpcNow() - g_runStartedQpc),
            JsonEscape(reason ? reason : "").c_str());
        fclose(g_report);
        g_report = nullptr;
    }
    strncpy_s(g_lastReportPath, g_reportPath, _TRUNCATE);
    g_phase = aborted ? BenchmarkPhase::Aborted : BenchmarkPhase::Complete;
    g_abortRequested.store(false);
    sprintf_s(g_message, "%s; runtime state restored", aborted ? "Benchmark aborted" : "Benchmark complete");
}

void FinishScenario(int64_t now) {
    const Scenario& scenario = g_scenarios[g_scenarioIndex];
    const double measuredSeconds = QpcSeconds(now - g_phaseStartedQpc);
    const ResourceSample resourceEnd = CaptureResourceSample();
    WriteScenarioResult(scenario, resourceEnd, measuredSeconds);
    g_completedPlannedSeconds += scenario.settleSeconds + scenario.measureSeconds;
    ++g_scenarioIndex;
    if (g_scenarioIndex >= g_scenarios.size()) {
        FinishRun(false, "all scenarios completed");
        return;
    }
    EnterScenario(now);
}

const char* PhaseLabel() {
    switch (g_phase) {
    case BenchmarkPhase::Countdown: return "Countdown";
    case BenchmarkPhase::Settling: return "Settling";
    case BenchmarkPhase::Measuring: return "Measuring";
    case BenchmarkPhase::Complete: return "Complete";
    case BenchmarkPhase::Aborted: return "Aborted";
    case BenchmarkPhase::Idle:
    default: return "Idle";
    }
}

} // namespace

bool PerformanceBenchmarkStart(char* outError, size_t outErrorSize) {
    if (outError && outErrorSize) outError[0] = '\0';
    if (g_devPerformanceBenchmarkActive.load()) {
        if (outError && outErrorSize) sprintf_s(outError, outErrorSize, "Benchmark is already running");
        return false;
    }
    if (g_addonStartupState.load() != AddonStartupState::Ready) {
        if (outError && outErrorSize) sprintf_s(outError, outErrorSize, "Crimson Weather startup is not ready");
        return false;
    }

    QueryPerformanceFrequency(&g_qpcFrequency);
    CaptureRuntimeSnapshot();
    BuildScenarios();
    if (g_scenarios.empty() || !OpenReport(outError, outErrorSize)) {
        return false;
    }

    g_scenarioIndex = 0;
    g_completedPlannedSeconds = 0.0;
    g_lastReportPath[0] = '\0';
    g_abortRequested.store(false);
    g_devPerformanceBenchmarkActive.store(true);
    g_phase = BenchmarkPhase::Countdown;
    g_runStartedQpc = QpcNow();
    g_phaseStartedQpc = g_runStartedQpc;
    sprintf_s(g_message, "Close the overlay; benchmark starts in %.0f seconds", kCountdownSeconds);
    Log("[perf] DEV performance benchmark started scenarios=%zu planned=%.1fmin report=%s\n",
        g_scenarios.size(), g_totalPlannedSeconds / 60.0, g_reportPath);
    return true;
}

void PerformanceBenchmarkAbort() {
    if (g_devPerformanceBenchmarkActive.load()) {
        g_abortRequested.store(true);
        strcpy_s(g_message, "Abort requested; restoring runtime state");
    }
}

void PerformanceBenchmarkOnPresent(ID3D12Device* device) {
    if (!g_devPerformanceBenchmarkActive.load()) {
        return;
    }
    ResolveAdapter(device);
    const int64_t now = QpcNow();
    if (g_abortRequested.load()) {
        FinishRun(true, "user abort");
        return;
    }

    if (g_phase == BenchmarkPhase::Countdown) {
        if (QpcSeconds(now - g_phaseStartedQpc) >= kCountdownSeconds) {
            WriteRunHeader();
            EnterScenario(now);
        }
        return;
    }
    if (g_scenarioIndex >= g_scenarios.size()) {
        FinishRun(false, "scenario index reached end");
        return;
    }

    const Scenario& scenario = g_scenarios[g_scenarioIndex];
    if (g_phase == BenchmarkPhase::Settling) {
        if (QpcSeconds(now - g_phaseStartedQpc) >= scenario.settleSeconds) {
            BeginMeasurement(now);
        }
        return;
    }
    if (g_phase != BenchmarkPhase::Measuring) {
        return;
    }

    const double elapsed = QpcSeconds(now - g_phaseStartedQpc);
    if (scenario.tick) {
        scenario.tick(elapsed);
    }
    if (g_lastFrameQpc != 0) {
        const double frameMs = QpcSeconds(now - g_lastFrameQpc) * 1000.0;
        if (frameMs > 0.0 && frameMs < 10000.0) {
            g_frameTimesMs.push_back(frameMs);
        }
    }
    g_lastFrameQpc = now;

    if (QpcSeconds(now - g_lastTelemetryQpc) >= kTelemetryIntervalSeconds) {
        g_telemetry.push_back({ elapsed, CaptureResourceSample() });
        g_lastTelemetryQpc = now;
        g_lastFrameQpc = 0;
        ++g_profilerIntervalsSkipped;
    }
    if (elapsed >= scenario.measureSeconds) {
        FinishScenario(now);
    }
}

void PerformanceBenchmarkShutdown() {
    if (g_devPerformanceBenchmarkActive.load()) {
        FinishRun(true, "addon shutdown");
    }
    if (g_adapter) {
        g_adapter->Release();
        g_adapter = nullptr;
    }
}

PerformanceBenchmarkStatus PerformanceBenchmarkGetStatus() {
    PerformanceBenchmarkStatus status{};
    status.active = g_devPerformanceBenchmarkActive.load();
    status.abortPending = g_abortRequested.load();
    status.scenarioIndex = min(g_scenarioIndex + (status.active ? 1u : 0u), g_scenarios.size());
    status.scenarioCount = g_scenarios.size();
    status.phase = PhaseLabel();
    status.scenario = g_scenarioIndex < g_scenarios.size() ? g_scenarios[g_scenarioIndex].id.c_str() : "";
    status.reportPath = g_lastReportPath[0] ? g_lastReportPath : g_reportPath;
    status.message = g_message;

    double currentSeconds = 0.0;
    if (status.active) {
        const int64_t now = QpcNow();
        if (g_phase == BenchmarkPhase::Countdown) {
            currentSeconds = min(kCountdownSeconds, QpcSeconds(now - g_phaseStartedQpc));
        } else if (g_scenarioIndex < g_scenarios.size()) {
            const Scenario& scenario = g_scenarios[g_scenarioIndex];
            const double phaseElapsed = QpcSeconds(now - g_phaseStartedQpc);
            currentSeconds = g_completedPlannedSeconds;
            if (g_phase == BenchmarkPhase::Settling) {
                currentSeconds += min(scenario.settleSeconds, phaseElapsed);
                status.scenarioProgress = scenario.settleSeconds > 0.0
                    ? static_cast<float>(0.5 * min(1.0, phaseElapsed / scenario.settleSeconds)) : 0.5f;
            } else if (g_phase == BenchmarkPhase::Measuring) {
                currentSeconds += scenario.settleSeconds + min(scenario.measureSeconds, phaseElapsed);
                status.scenarioProgress = static_cast<float>(0.5 + 0.5 * min(1.0, phaseElapsed / scenario.measureSeconds));
            }
        }
    } else if (g_phase == BenchmarkPhase::Complete) {
        currentSeconds = g_totalPlannedSeconds;
        status.scenarioProgress = 1.0f;
    }
    status.overallProgress = g_totalPlannedSeconds > 0.0
        ? static_cast<float>(min(1.0, currentSeconds / g_totalPlannedSeconds)) : 0.0f;
    status.estimatedSecondsRemaining = static_cast<unsigned int>(max(0.0, g_totalPlannedSeconds - currentSeconds));
    return status;
}

#endif
