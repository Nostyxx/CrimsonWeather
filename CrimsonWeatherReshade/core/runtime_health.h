#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

enum class RuntimeHealthState : uint8_t {
    Disabled = 0,
    Degraded = 1,
    Ready = 2,
};

enum class AobTargetId : uint8_t {
    WeatherTick = 0,
    WeatherCompose,
    GetRainIntensity,
    GetSnowIntensity,
    GetDustIntensity,
    ProcessWindState,
    ActivateEffect,
    SetIntensity,
    WindPack,
    SceneFrameUpdate,
    EnvManagerPtr,
    NullSentinel,
    TimeStores,
    TimeDebugHandler,
    NativeToast,
    MinimapRegionLabels,
    MinimapGameTimeUpdate,
    GameTimeGetter,
    Count
};

enum class RuntimeHealthGroup : uint8_t {
    CoreWeather = 0,
    CloudExperiment,
    Fog,
    Time,
    Infra,
    Count
};

enum class RuntimeFeatureId : uint8_t {
    ForceClear = 0,
    Rain,
    ThunderControls,
    Dust,
    Snow,
    TimeControls,
    CloudControls,
    FogControls,
    WindControls,
    NoWindControls,
    DetailControls,
    ExperimentControls,
    CelestialControls,
    NativeToast,
    Count
};

struct RuntimeHealthEntry {
    RuntimeHealthState state = RuntimeHealthState::Disabled;
    uintptr_t addr = 0;
    std::string note;
};

extern std::array<RuntimeHealthEntry, static_cast<size_t>(AobTargetId::Count)> g_aobTargetHealth;
extern std::array<RuntimeHealthEntry, static_cast<size_t>(RuntimeHealthGroup::Count)> g_runtimeGroupHealth;
extern std::array<RuntimeHealthEntry, static_cast<size_t>(RuntimeFeatureId::Count)> g_runtimeFeatureHealth;

const char* RuntimeHealthStateLabel(RuntimeHealthState state);
const char* AobTargetLabel(AobTargetId id);
const char* RuntimeHealthGroupLabel(RuntimeHealthGroup id);
const char* RuntimeFeatureLabel(RuntimeFeatureId id);
void ClearRuntimeHealthState();
void SetAobTargetHealth(AobTargetId id, RuntimeHealthState state, uintptr_t addr, const std::string& note);
void SetRuntimeGroupHealth(RuntimeHealthGroup id, RuntimeHealthState state, const std::string& note);
void SetRuntimeFeatureHealth(RuntimeFeatureId id, RuntimeHealthState state, const std::string& note);
RuntimeHealthState GetRuntimeFeatureState(RuntimeFeatureId id);
bool RuntimeFeatureAvailable(RuntimeFeatureId id);
const char* RuntimeFeatureNote(RuntimeFeatureId id);
bool RuntimeStartupHealthy(char* outReason, size_t outReasonSize);
