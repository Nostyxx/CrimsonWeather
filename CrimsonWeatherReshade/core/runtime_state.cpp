#include "pch.h"

#include "runtime_shared.h"

#include <mutex>

namespace {

std::mutex g_nativeToastQueueMutex;
char g_queuedNativeToast[128] = {};
std::atomic_bool g_nativeToastQueued = false;

} // namespace

void GUI_SetStatus(const char* msg) {
    if (!msg) {
        return;
    }
    strncpy_s(g_statusText, msg, _TRUNCATE);
}

void ResetAllSliders() {
    g_oRain.clear();
    g_oThunder.clear();
    g_oSnow.clear();
    g_oDust.clear();
    g_oSnowAccumBoundaryA.clear();
    g_oSnowAccumBoundaryB.clear();
    g_oSnowCoverageThreshold.clear();
    g_snowCoverageGlobalsDirty.store(true);
    g_oCloudAmount.clear();
    g_oCloudSpdX.clear();
    g_oCloudSpdY.clear();
    g_oHighClouds.clear();
    g_oAtmoAlpha.clear();
    g_oExpCloud2C.clear();
    g_oExpCloud2D.clear();
    g_oCloudVariation.clear();
    g_oExpNightSkyRot.clear();
    g_oNightSkyYaw.clear();
    g_oCloudThk.clear();
    g_oNativeFog.clear();
    g_oWind.clear();
    g_oWindActual.clear();
    g_oSunDirX.clear();
    g_oSunDirY.clear();
    g_oMoonDirX.clear();
    g_oMoonDirY.clear();
    g_oMoonRoll.clear();
    g_oSunSize.clear();
    g_oMoonSize.clear();
    g_oSunLightIntensity.clear();
    g_oMoonLightIntensity.clear();
    g_oMieScaleHeight.clear();
    g_oMieAerosolDensity.clear();
    g_oMieAerosolAbsorption.clear();
    g_oHeightFogBaseline.clear();
    g_oHeightFogFalloff.clear();
    g_oCloudAlpha.clear();
    g_oCloudFadeRange.clear();
    g_oCloudDetailRatio.clear();
    g_oCloudPhaseFront.clear();
    g_oCloudScatteringCoefficient.clear();
    g_oCloudFlow.clear();
    g_oCloudVisibleRange.clear();
    g_oRayleighHeight.clear();
    g_oOzoneRatio.clear();
    g_oRayleighScatteringColor.clear();
    g_oVolumeFogScatterColor.clear();
    g_oMieScatterColor.clear();
    g_forceClear.store(false);
    g_noRain.store(false);
    g_noDust.store(false);
    g_noSnow.store(false);
    g_noWind.store(false);
    g_noFog.store(false);
    g_windMul.store(1.0f);
    g_timeCtrlActive.store(false);
    g_timeFreeze.store(false);
    g_timeProgressVisualTime.store(false);
    g_timeProgressMatchGameTime.store(false);
    g_timeProgressLastTick.store(0);
    g_timeProgressMatchLastMinute.store(-1);
    g_timeProgressMatchPendingMs.store(0);
    g_realGameTimeEnabled.store(false);
    g_cfg.realGameTimeEnabled = false;
    g_realGameTimeDayScale.store(1.0f);
    g_realGameTimeNightScale.store(1.0f);
    g_cfg.realGameTimeDayScale = 1.0f;
    g_cfg.realGameTimeNightScale = 1.0f;
    g_realGameTimeSetMinuteRequest.store(-1);
    g_realGameTimeDayDeltaRequest.store(0);
    g_timeApplyRequest.store(false);
    g_timeTargetHour.store(g_timeCurrentHour.load());
    g_timeOriginalHour.store(g_timeCurrentHour.load());
    g_timeOriginalHourValid.store(false);
    g_timeSetHoldTicks.store(0);
    g_timeFrozenRaw.store(-9999.0f);
    g_windPackBaseValid.store(false);
    g_windPackBase32Valid.store(false);
    g_windPackBase32.store(0.0f);
    g_windPackBase2CValid.store(false);
    g_windPackBase2C.store(0.0f);
    g_windPackBase2DValid.store(false);
    g_windPackBase2D.store(0.0f);
    g_windPackBase0AValid.store(false);
    g_windPackBase0A.store(0.0f);
    g_windPackBase0BValid.store(false);
    g_windPackBase0B.store(0.0f);
    g_windPackBase11Valid.store(false);
    g_windPackBase11.store(0.0f);
    g_windPackBase17Valid.store(false);
    g_windPackBase17.store(0.0f);
    g_windPackBase1BValid.store(false);
    g_windPackBase1B.store(0.0f);
    g_windPackBase00Valid.store(false);
    g_windPackBase00.store(1.0f);
    g_windPackBase05Valid.store(false);
    g_windPackBase05.store(1.0f);
    g_windPackBase0FValid.store(false);
    g_windPackBase0FBits.store(0xFFFFFFu);
    g_windPackBase0EValid.store(false);
    g_windPackBase0E.store(1200.0f);
    g_windPackBase14Valid.store(false);
    g_windPackBase14.store(0.0f);
    g_windPackBase10Valid.store(false);
    g_windPackBase10.store(1200.0f);
    g_windPackBase12Valid.store(false);
    g_windPackBase12.store(0.0f);
    g_windPackBase18Valid.store(false);
    g_windPackBase18.store(0.0f);
    g_windPackBase19Valid.store(false);
    g_windPackBase19.store(0.0f);
    g_windPackBase1EValid.store(false);
    g_windPackBase1E.store(1.0f);
    g_windPackBase1FValid.store(false);
    g_windPackBase1F.store(1.0f);
    g_windPackBase20Valid.store(false);
    g_windPackBase20.store(0.0f);
    g_windPackBase21Valid.store(false);
    g_windPackBase21.store(0.0f);
    g_windPackBase25Valid.store(false);
    g_windPackBase25.store(0.0f);
    g_windPackBase27Valid.store(false);
    g_windPackBase27.store(0.0f);
    g_windPackBase28Valid.store(false);
    g_windPackBase28.store(0.0f);
    g_windPackBaseVolumeFogColorValid.store(false);
    g_windPackBase34.store(1.0f);
    g_windPackBase35.store(1.0f);
    g_windPackBase36.store(1.0f);
    g_windPackBase37.store(1.0f);
    g_windPackBaseMieScatterColorValid.store(false);
    g_windPackBase38.store(1.0f);
    g_windPackBase39.store(1.0f);
    g_windPackBase3A.store(1.0f);
    g_windPackBase3B.store(1.0f);
    g_windNodeBaseValid.store(false);
    g_windNodeBaseSpeed.store(0.0f);
    g_windNodeBaseGust.store(0.0f);
    g_atmoCelestialBaseValid.store(false);
    g_atmoBaseSunSize.store(0.267f);
    g_atmoBaseMoonSize.store(0.267f);
    g_sceneCelestialBaseValid.store(false);
    g_sceneBaseSunYaw.store(0.0f);
    g_sceneBaseSunPitch.store(0.0f);
    g_sceneBaseMoonYaw.store(0.0f);
    g_sceneBaseMoonPitch.store(0.0f);
    g_sceneBaseNightSkyYaw.store(0.0f);
    SaveGeneralConfig();
    g_resetStopRequested.store(true);
}

bool AnyCustomWeatherSliderActive() {
    return g_forceClear.load() ||
           g_oRain.active.load() || g_oSnow.active.load() || g_oDust.active.load() ||
           g_oSnowAccumBoundaryA.active.load() || g_oSnowAccumBoundaryB.active.load() ||
           g_oSnowCoverageThreshold.active.load() ||
           g_oCloudAmount.active.load() ||
           g_oCloudSpdX.active.load() || g_oCloudSpdY.active.load() || g_oHighClouds.active.load() ||
           g_oAtmoAlpha.active.load() || g_oCloudThk.active.load() || g_oNativeFog.active.load() ||
           g_oCloudVariation.active.load() ||
           g_oSunLightIntensity.active.load() || g_oMoonLightIntensity.active.load() ||
           g_oMieScaleHeight.active.load() || g_oMieAerosolDensity.active.load() || g_oMieAerosolAbsorption.active.load() ||
           g_oHeightFogBaseline.active.load() || g_oHeightFogFalloff.active.load() ||
           g_oCloudAlpha.active.load() || g_oCloudFadeRange.active.load() || g_oCloudDetailRatio.active.load() ||
           g_oCloudPhaseFront.active.load() || g_oCloudScatteringCoefficient.active.load() || g_oCloudFlow.active.load() ||
           g_oCloudVisibleRange.active.load() ||
           g_oRayleighHeight.active.load() || g_oOzoneRatio.active.load() ||
           g_oRayleighScatteringColor.active.load() || g_oVolumeFogScatterColor.active.load() || g_oMieScatterColor.active.load() ||
           g_noRain.load() || g_noDust.load() || g_noSnow.load() ||
           g_noFog.load() || g_noWind.load() ||
           g_oWindActual.active.load() ||
           fabsf(g_windMul.load() - 1.0f) > 0.001f;
}

void SetModEnabled(bool enabled) {
    const bool wasEnabled = g_modEnabled.exchange(enabled);
    if (wasEnabled == enabled) {
        return;
    }

    if (!enabled) {
        g_modSuspendRequested.store(true);
        GUI_SetStatus("Weather control disabled");
        Log("[i] Weather control disabled\n");
        ShowNativeToast("CRIMSON WEATHER DISABLED");
        return;
    }

    GUI_SetStatus("Weather control enabled");
    Log("[i] Weather control enabled\n");
    ShowNativeToast("CRIMSON WEATHER ENABLED");
}

void ToggleModEnabled() {
    SetModEnabled(!g_modEnabled.load());
}

const char* AddonStartupStateLabel(AddonStartupState state) {
    switch (state) {
    case AddonStartupState::Starting:
        return "Starting";
    case AddonStartupState::Ready:
        return "Ready";
    case AddonStartupState::Failed:
        return "Failed";
    default:
        return "Not started";
    }
}

const char* StartupStepLabel(StartupStepId step) {
    switch (step) {
    case StartupStepId::Config:
        return "Config";
    case StartupStepId::MinHook:
        return "Hook engine";
    case StartupStepId::AobScan:
        return "AOB scan";
    case StartupStepId::Presets:
        return "Presets";
    case StartupStepId::Hotkeys:
        return "Hotkeys";
    case StartupStepId::Ready:
        return "Ready";
    case StartupStepId::Failed:
        return "Failed";
    default:
        return "Idle";
    }
}

void StartupAppendLog(const char* level, const char* msg) {
    const unsigned int seq = g_startupLogSequence.fetch_add(1);
    const int slot = static_cast<int>(seq % kStartupLogLineCount);
    const char* safeLevel = (level && level[0]) ? level : "i";
    const char* safeMsg = msg ? msg : "";
    sprintf_s(g_startupLogLines[slot], "[%s] %s", safeLevel, safeMsg);
}

void StartupSetStep(StartupStepId step, int index, const char* detail) {
    g_startupStep.store(step);
    g_startupStepIndex.store(max(0, min(g_startupStepCount.load(), index)));
    if (detail && detail[0]) {
        strncpy_s(g_startupDetailText, detail, _TRUNCATE);
        StartupAppendLog(step == StartupStepId::Failed ? "fail" : "run", detail);
        GUI_SetStatus(detail);
    }
    if (step == StartupStepId::Ready || step == StartupStepId::Failed) {
        g_startupEndTick.store(GetTickCount64());
    }
}

void StartupResetProgress() {
    g_startupStep.store(StartupStepId::Idle);
    g_startupStepIndex.store(0);
    g_startupStepCount.store(6);
    g_startupStartTick.store(0);
    g_startupEndTick.store(0);
    g_startupLogSequence.store(0);
    strcpy_s(g_startupDetailText, "Waiting for user");
    for (auto& line : g_startupLogLines) {
        line[0] = '\0';
    }
}

void* ResolveNativeToastManager() {
    if (!g_pNativeToastRootGlobal) {
        return nullptr;
    }

    void* root = *g_pNativeToastRootGlobal;
    if (!root || g_nativeToastOuterOffset == 0 || g_nativeToastManagerOffset == 0) {
        return nullptr;
    }

    __try {
        void* outer = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(root) + g_nativeToastOuterOffset);
        if (!outer) {
            return nullptr;
        }
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(outer) + g_nativeToastManagerOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool NativeToastReady() {
    const bool legacyBridgeReady =
        g_pNativeToastCreateString && g_pNativeToastPush && g_pNativeToastReleaseString &&
        g_pNativeToastRootGlobal && g_nativeToastOuterOffset != 0 && g_nativeToastManagerOffset != 0;
    return legacyBridgeReady || g_pNativeToastShowAlert;
}

static void* ResolveToastUiRootNoThrow(void* minimapUi) {
    __try {
        void** vtable = *reinterpret_cast<void***>(minimapUi);
        if (!vtable || !vtable[34]) {
            return nullptr;
        }
        using GetUiRoot_fn = void*(__fastcall*)(void*);
        return reinterpret_cast<GetUiRoot_fn>(vtable[34])(minimapUi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

static bool ShowAlertNoThrow(void* uiRoot, const char* message) {
    __try {
        g_pNativeToastShowAlert(uiRoot, message, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ShowNativeToast(const char* msg) {
    if (!g_cfg.toastNotification || !msg || !msg[0] || !NativeToastReady()) {
        return;
    }

    if (g_pNativeToastShowAlert) {
        std::lock_guard<std::mutex> lock(g_nativeToastQueueMutex);
        strncpy_s(g_queuedNativeToast, msg, _TRUNCATE);
        g_nativeToastQueued = true;
        return;
    }

    void* manager = ResolveNativeToastManager();
    if (!manager) {
        Log("[W] native toast manager unavailable for: %s\n", msg);
        return;
    }
    void* messageHandle = g_pNativeToastCreateString(msg);
    if (!messageHandle) {
        Log("[W] native toast string create failed: %s\n", msg);
        return;
    }

    g_pNativeToastPush(manager, &messageHandle, 0);
    g_pNativeToastReleaseString(messageHandle);
    Log("[i] native toast shown: %s\n", msg);
}

void FlushQueuedNativeToast(void* minimapUi) {
    if (!g_pNativeToastShowAlert || !minimapUi || !g_nativeToastQueued.load()) {
        return;
    }

    void* uiRoot = ResolveToastUiRootNoThrow(minimapUi);
    if (!uiRoot) {
        return;
    }

    char message[sizeof(g_queuedNativeToast)] = {};
    {
        std::lock_guard<std::mutex> lock(g_nativeToastQueueMutex);
        if (!g_nativeToastQueued) {
            return;
        }
        strcpy_s(message, g_queuedNativeToast);
        g_nativeToastQueued = false;
        g_queuedNativeToast[0] = '\0';
    }

    if (ShowAlertNoThrow(uiRoot, message)) {
        Log("[i] native toast shown: %s\n", message);
    } else {
        Log("[W] native toast alert exception: %s\n", message);
    }
}
