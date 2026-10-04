#include "pch.h"
#include "runtime_shared.h"

std::array<RuntimeHealthEntry, static_cast<size_t>(AobTargetId::Count)> g_aobTargetHealth{};
std::array<RuntimeHealthEntry, static_cast<size_t>(RuntimeHealthGroup::Count)> g_runtimeGroupHealth{};
std::array<RuntimeHealthEntry, static_cast<size_t>(RuntimeFeatureId::Count)> g_runtimeFeatureHealth{};

const char* RuntimeHealthStateLabel(RuntimeHealthState state) {
    switch (state) {
    case RuntimeHealthState::Ready:
        return "READY";
    case RuntimeHealthState::Degraded:
        return "DEGRADED";
    default:
        return "DISABLED";
    }
}

const char* AobTargetLabel(AobTargetId id) {
    switch (id) {
    case AobTargetId::WeatherTick:
        return "WeatherTick";
    case AobTargetId::WeatherCompose:
        return "WeatherCompose";
    case AobTargetId::GetRainIntensity:
        return "GetRainIntensity";
    case AobTargetId::GetSnowIntensity:
        return "GetSnowIntensity";
    case AobTargetId::GetDustIntensity:
        return "GetDustIntensity";
    case AobTargetId::ProcessWindState:
        return "ProcessWindState";
    case AobTargetId::ActivateEffect:
        return "ActivateEffect";
    case AobTargetId::SetIntensity:
        return "SetIntensity";
    case AobTargetId::WindPack:
        return "WindPack";
    case AobTargetId::SceneFrameUpdate:
        return "SceneFrameUpdate";
    case AobTargetId::EnvManagerPtr:
        return "EnvManagerPtr";
    case AobTargetId::NullSentinel:
        return "NullSentinel";
    case AobTargetId::TimeStores:
        return "TimeStores";
    case AobTargetId::TimeDebugHandler:
        return "TimeDebugHandler";
    case AobTargetId::NativeToast:
        return "NativeToast";
    case AobTargetId::MinimapRegionLabels:
        return "MinimapRegionLabels";
    case AobTargetId::MinimapGameTimeUpdate:
        return "MinimapGameTimeUpdate";
    case AobTargetId::GameTimeGetter:
        return "GameTimeGetter";
    default:
        return "UnknownTarget";
    }
}

const char* RuntimeHealthGroupLabel(RuntimeHealthGroup id) {
    switch (id) {
    case RuntimeHealthGroup::CoreWeather:
        return "CoreWeather";
    case RuntimeHealthGroup::CloudExperiment:
        return "CloudExperiment";
    case RuntimeHealthGroup::Fog:
        return "Fog";
    case RuntimeHealthGroup::Time:
        return "Time";
    case RuntimeHealthGroup::Infra:
        return "Infra";
    default:
        return "UnknownGroup";
    }
}

const char* RuntimeFeatureLabel(RuntimeFeatureId id) {
    switch (id) {
    case RuntimeFeatureId::ForceClear:
        return "ForceClear";
    case RuntimeFeatureId::Rain:
        return "Rain";
    case RuntimeFeatureId::ThunderControls:
        return "ThunderControls";
    case RuntimeFeatureId::Dust:
        return "Dust";
    case RuntimeFeatureId::Snow:
        return "Snow";
    case RuntimeFeatureId::TimeControls:
        return "TimeControls";
    case RuntimeFeatureId::CloudControls:
        return "CloudControls";
    case RuntimeFeatureId::FogControls:
        return "FogControls";
    case RuntimeFeatureId::WindControls:
        return "WindControls";
    case RuntimeFeatureId::NoWindControls:
        return "NoWindControls";
    case RuntimeFeatureId::DetailControls:
        return "DetailControls";
    case RuntimeFeatureId::ExperimentControls:
        return "ExperimentControls";
    case RuntimeFeatureId::CelestialControls:
        return "CelestialControls";
    case RuntimeFeatureId::NativeToast:
        return "NativeToast";
    default:
        return "UnknownFeature";
    }
}

void ClearRuntimeHealthState() {
    for (auto& entry : g_aobTargetHealth) {
        entry = RuntimeHealthEntry{};
    }
    for (auto& entry : g_runtimeGroupHealth) {
        entry = RuntimeHealthEntry{};
    }
    for (auto& entry : g_runtimeFeatureHealth) {
        entry = RuntimeHealthEntry{};
    }
}

void SetAobTargetHealth(AobTargetId id, RuntimeHealthState state, uintptr_t addr, const std::string& note) {
    RuntimeHealthEntry& entry = g_aobTargetHealth[static_cast<size_t>(id)];
    entry.state = state;
    entry.addr = addr;
    entry.note = note;
}

void SetRuntimeGroupHealth(RuntimeHealthGroup id, RuntimeHealthState state, const std::string& note) {
    RuntimeHealthEntry& entry = g_runtimeGroupHealth[static_cast<size_t>(id)];
    entry.state = state;
    entry.addr = 0;
    entry.note = note;
}

void SetRuntimeFeatureHealth(RuntimeFeatureId id, RuntimeHealthState state, const std::string& note) {
    RuntimeHealthEntry& entry = g_runtimeFeatureHealth[static_cast<size_t>(id)];
    entry.state = state;
    entry.addr = 0;
    entry.note = note;
}

RuntimeHealthState GetRuntimeFeatureState(RuntimeFeatureId id) {
    return g_runtimeFeatureHealth[static_cast<size_t>(id)].state;
}

bool RuntimeFeatureAvailable(RuntimeFeatureId id) {
    return GetRuntimeFeatureState(id) != RuntimeHealthState::Disabled;
}

const char* RuntimeFeatureNote(RuntimeFeatureId id) {
    return g_runtimeFeatureHealth[static_cast<size_t>(id)].note.c_str();
}

bool RuntimeStartupHealthy(char* outReason, size_t outReasonSize) {
    if (outReason && outReasonSize > 0) {
        outReason[0] = '\0';
    }

#if defined(CW_DEV_BUILD)
    const DevLaunchOption option = g_devLaunchOption.load();
    if (DevLaunchOptionBypassesStartupHealth(option)) {
        Log("[dev] Startup health bypassed for LaunchOption=%s\n", DevLaunchOptionName(option));
        return true;
    }
#endif

#if defined(CW_WIND_ONLY)
    constexpr RuntimeFeatureId criticalFeatures[] = {
        RuntimeFeatureId::WindControls
    };
#else
    constexpr RuntimeFeatureId criticalFeatures[] = {
        RuntimeFeatureId::ForceClear,
        RuntimeFeatureId::Rain,
        RuntimeFeatureId::Dust,
        RuntimeFeatureId::Snow,
        RuntimeFeatureId::CloudControls,
        RuntimeFeatureId::FogControls,
        RuntimeFeatureId::WindControls,
        RuntimeFeatureId::NoWindControls,
        RuntimeFeatureId::DetailControls,
        RuntimeFeatureId::ExperimentControls
    };
#endif

    for (RuntimeFeatureId feature : criticalFeatures) {
        const RuntimeHealthEntry& entry = g_runtimeFeatureHealth[static_cast<size_t>(feature)];
        if (entry.state == RuntimeHealthState::Ready) {
            continue;
        }
        if (outReason && outReasonSize > 0) {
            const char* note = entry.note.empty() ? "hook missing" : entry.note.c_str();
            sprintf_s(outReason, outReasonSize, "%s %s: %s",
                RuntimeFeatureLabel(feature),
                RuntimeHealthStateLabel(entry.state),
                note);
        }
        return false;
    }

    return true;
}
