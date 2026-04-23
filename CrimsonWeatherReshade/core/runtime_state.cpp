#include "pch.h"

#include "runtime_shared.h"

namespace {

void WriteDefaultConfig(const char* path) {
    if (!path || !path[0]) {
        return;
    }

    WritePrivateProfileStringA("General", "LogEnabled", "0", path);
    WritePrivateProfileStringA("General", "HotkeyToggleEffect", "F10", path);
    WritePrivateProfileStringA(
        "General",
        "_HotkeyOptions",
        "F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, or single letter A-Z",
        path);
    WritePrivateProfileStringA("Hotkeys", "ControllerToggleEffect", "dpad_down+a", path);
    WritePrivateProfileStringA(
        "Hotkeys",
        "_ControllerHotkeyOptions",
        "Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back",
        path);
    WritePrivateProfileStringA("Preset", "LastPreset", "", path);
}

void PatchMissingConfigKeys(const char* path) {
    if (!path || !path[0]) {
        return;
    }

    char buf[64] = {};
    if (GetPrivateProfileStringA("General", "HotkeyToggleEffect", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("General", "HotkeyToggleEffect", "F10", path);
    }
    if (GetPrivateProfileStringA("General", "_HotkeyOptions", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA(
            "General",
            "_HotkeyOptions",
            "F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, or single letter A-Z",
            path);
    }
    if (GetPrivateProfileStringA("Hotkeys", "ControllerToggleEffect", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("Hotkeys", "ControllerToggleEffect", "dpad_down+a", path);
    }
    if (GetPrivateProfileStringA("Hotkeys", "_ControllerHotkeyOptions", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA(
            "Hotkeys",
            "_ControllerHotkeyOptions",
            "Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back",
            path);
    }
    GetPrivateProfileStringA("Preset", "LastPreset", "", buf, sizeof(buf), path);
    WritePrivateProfileStringA("Preset", "LastPreset", buf, path);

    WritePrivateProfileStringA("General", "HotkeyToggleGUI", nullptr, path);
    WritePrivateProfileStringA("Hotkeys", "ControllerHotkeyToggleGUI", nullptr, path);
    WritePrivateProfileStringA("UI", "Scale", nullptr, path);
    WritePrivateProfileStringA("UI", "ShowOnStartup", nullptr, path);
    WritePrivateProfileStringA("Diagnostics", "ReShadeVerbose", nullptr, path);
}

} // namespace

void Log(const char* fmt, ...) {
    if (!g_logEnabled || !g_logFile) {
        return;
    }

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    fprintf(g_logFile, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fflush(g_logFile);
}

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
    case AobTargetId::CloudPack:
        return "CloudPack";
    case AobTargetId::WindPack:
        return "WindPack";
    case AobTargetId::PostProcessLayerUpdate:
        return "PostProcessLayerUpdate";
    case AobTargetId::GetLayerMeta:
        return "GetLayerMeta";
    case AobTargetId::WeatherFrameUpdate:
        return "WeatherFrameUpdate";
    case AobTargetId::AtmosFogBlend:
        return "AtmosFogBlend";
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

bool RuntimeFeatureDegraded(RuntimeFeatureId id) {
    return GetRuntimeFeatureState(id) == RuntimeHealthState::Degraded;
}

const char* RuntimeFeatureNote(RuntimeFeatureId id) {
    return g_runtimeFeatureHealth[static_cast<size_t>(id)].note.c_str();
}

float ClampUiScale(float v) {
    return min(kUiScaleMax, max(kUiScaleMin, v));
}

void BuildIniPath(char* outPath, size_t outSize) {
    if (!outPath || outSize == 0) {
        return;
    }
    if (g_pluginDir[0]) {
        sprintf_s(outPath, outSize, "%s\\CrimsonWeather.ini", g_pluginDir);
        return;
    }
    strcpy_s(outPath, outSize, "CrimsonWeather.ini");
}

int KeyNameToVK(const char* name) {
    if (!name || !name[0]) {
        return VK_F10;
    }
    if (!_stricmp(name, "F1")) return VK_F1;
    if (!_stricmp(name, "F2")) return VK_F2;
    if (!_stricmp(name, "F3")) return VK_F3;
    if (!_stricmp(name, "F4")) return VK_F4;
    if (!_stricmp(name, "F5")) return VK_F5;
    if (!_stricmp(name, "F6")) return VK_F6;
    if (!_stricmp(name, "F7")) return VK_F7;
    if (!_stricmp(name, "F8")) return VK_F8;
    if (!_stricmp(name, "F9")) return VK_F9;
    if (!_stricmp(name, "F10")) return VK_F10;
    if (!_stricmp(name, "F11")) return VK_F11;
    if (!_stricmp(name, "F12")) return VK_F12;
    if (!_stricmp(name, "INSERT")) return VK_INSERT;
    if (!_stricmp(name, "DELETE")) return VK_DELETE;
    if (!_stricmp(name, "HOME")) return VK_HOME;
    if (!_stricmp(name, "END")) return VK_END;
    if (!_stricmp(name, "PGUP")) return VK_PRIOR;
    if (!_stricmp(name, "PGDN")) return VK_NEXT;
    if (strlen(name) == 1) return toupper(name[0]);
    return VK_F10;
}

WORD ControllerTokenToMask(const char* token) {
    if (!token || !token[0]) {
        return 0;
    }
    if (!_stricmp(token, "dpad_up") || !_stricmp(token, "up")) return 0x0001;
    if (!_stricmp(token, "dpad_down") || !_stricmp(token, "down")) return 0x0002;
    if (!_stricmp(token, "dpad_left") || !_stricmp(token, "left")) return 0x0004;
    if (!_stricmp(token, "dpad_right") || !_stricmp(token, "right")) return 0x0008;
    if (!_stricmp(token, "start") || !_stricmp(token, "options")) return 0x0010;
    if (!_stricmp(token, "back") || !_stricmp(token, "select") || !_stricmp(token, "share")) return 0x0020;
    if (!_stricmp(token, "lb") || !_stricmp(token, "l1")) return 0x0100;
    if (!_stricmp(token, "rb") || !_stricmp(token, "r1")) return 0x0200;
    if (!_stricmp(token, "a") || !_stricmp(token, "cross")) return 0x1000;
    if (!_stricmp(token, "b") || !_stricmp(token, "circle")) return 0x2000;
    if (!_stricmp(token, "x") || !_stricmp(token, "square")) return 0x4000;
    if (!_stricmp(token, "y") || !_stricmp(token, "triangle")) return 0x8000;
    return 0;
}

WORD ParseControllerCombo(const char* text, WORD fallback) {
    if (!text || !text[0]) {
        return fallback;
    }

    char copy[128] = {};
    strncpy_s(copy, text, _TRUNCATE);
    WORD mask = 0;
    char* context = nullptr;
    for (char* token = strtok_s(copy, "+|, ", &context); token; token = strtok_s(nullptr, "+|, ", &context)) {
        mask = static_cast<WORD>(mask | ControllerTokenToMask(token));
    }
    return mask ? mask : fallback;
}

bool IsControllerComboPressed(WORD buttons, WORD comboMask) {
    return comboMask != 0 && (buttons & comboMask) == comboMask;
}

void LoadConfig(const char* dir) {
    if (dir && dir[0]) {
        strcpy_s(g_pluginDir, dir);
    }

    char path[MAX_PATH] = {};
    BuildIniPath(path, sizeof(path));
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        WriteDefaultConfig(path);
    } else {
        PatchMissingConfigKeys(path);
    }

    char buf[64] = {};
    GetPrivateProfileStringA("General", "LogEnabled", "1", buf, sizeof(buf), path);
    g_cfg.logEnabled = atoi(buf) != 0;
    GetPrivateProfileStringA("General", "HotkeyToggleEffect", "F10", buf, sizeof(buf), path);
    g_cfg.effectToggleVK = KeyNameToVK(buf);
    GetPrivateProfileStringA("Hotkeys", "ControllerToggleEffect", "dpad_down+a", buf, sizeof(buf), path);
    g_cfg.controllerEffectToggleMask = ParseControllerCombo(buf, static_cast<WORD>(0x0002 | 0x4000));
    g_cfg.uiScale = 1.0f;
    g_cfg.reshadeDiagnostics = false;
}

void SaveConfigUIScale() {
}

void OpenLogFile(const char* dir) {
    if (!g_cfg.logEnabled) {
        g_logEnabled = false;
        return;
    }

    char path[MAX_PATH] = {};
    if (dir && dir[0]) {
        sprintf_s(path, "%s\\CrimsonWeather.log", dir);
    } else {
        strcpy_s(path, "CrimsonWeather.log");
    }
    fopen_s(&g_logFile, path, "w");
    if (!g_logFile) {
        g_logEnabled = false;
    }
}

void GUI_SetStatus(const char* msg) {
    if (!msg) {
        return;
    }
    strncpy_s(g_statusText, msg, _TRUNCATE);
}

void ResetAllSliders() {
    g_oRain.clear();
    g_oSnow.clear();
    g_oDust.clear();
    g_oFog.clear();
    g_forceCloudsEnabled.store(false);
    g_forceCloudsAmount.store(kForceCloudsDefaultAmount);
    g_oCloudSpdX.clear();
    g_oCloudSpdY.clear();
    g_oHighClouds.clear();
    g_oAtmoAlpha.clear();
    g_oExpCloud2C.clear();
    g_oExpCloud2D.clear();
    g_oExpNightSkyRot.clear();
    g_oCloudThk.clear();
    g_oWind.clear();
    g_oWindActual.clear();
    g_oSunDirX.clear();
    g_oSunDirY.clear();
    g_oMoonDirX.clear();
    g_oMoonDirY.clear();
    g_noWind.store(false);
    g_windMul.store(1.0f);
    g_timeCtrlActive.store(false);
    g_timeFreeze.store(false);
    g_timeApplyRequest.store(false);
    g_timeTargetHour.store(g_timeCurrentHour.load());
    g_timeOriginalHour.store(g_timeCurrentHour.load());
    g_timeOriginalHourValid.store(false);
    g_timeSetHoldTicks.store(0);
    g_timeFrozenRaw.store(-9999.0f);
    g_cloudBaseValid.store(false);
    g_windPackBaseValid.store(false);
    g_windPackBase11Valid.store(false);
    g_windPackBase11.store(0.0f);
    g_windPackBase17Valid.store(false);
    g_windPackBase17.store(0.0f);
    g_windNodeBaseValid.store(false);
    g_windNodeBaseSpeed.store(0.0f);
    g_windNodeBaseGust.store(0.0f);
    g_resetStopRequested.store(true);
}

bool AnySliderActive() {
    return g_oRain.active.load() || g_oSnow.active.load() || g_oDust.active.load() || g_oFog.active.load() ||
           g_oCloudSpdX.active.load() || g_oCloudSpdY.active.load() || g_oHighClouds.active.load() ||
           g_oAtmoAlpha.active.load() || g_oExpCloud2C.active.load() || g_oExpCloud2D.active.load() ||
           g_oExpNightSkyRot.active.load() || g_oCloudThk.active.load() || g_oWind.active.load() ||
           fabsf(g_windMul.load() - 1.0f) > 0.001f ||
           g_oSunDirX.active.load() || g_oSunDirY.active.load() ||
           g_oMoonDirX.active.load() || g_oMoonDirY.active.load();
}

bool AnyCustomWeatherSliderActive() {
    return g_oRain.active.load() || g_oSnow.active.load() || g_oDust.active.load() || g_oFog.active.load() ||
           g_oCloudSpdX.active.load() || g_oCloudSpdY.active.load() || g_oHighClouds.active.load() ||
           g_oAtmoAlpha.active.load() || g_oCloudThk.active.load() || g_oWindActual.active.load() ||
           fabsf(g_windMul.load() - 1.0f) > 0.001f;
}

namespace {

bool IsReadablePointer(uintptr_t addr, size_t bytes) {
    if (!addr || bytes == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    if (mbi.State != MEM_COMMIT) {
        return false;
    }

    const DWORD mask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & mask) == 0 || (mbi.Protect & PAGE_GUARD) != 0) {
        return false;
    }

    const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const auto end = base + mbi.RegionSize;
    return addr >= base && (addr + bytes) <= end;
}

bool InRange(float value, float lo, float hi) {
    return std::isfinite(value) && value >= lo && value <= hi;
}

bool LooksLikeAtmosphereConst0(long long candidate) {
    if (candidate <= 0 || candidate < 0x100000000LL ||
        !IsReadablePointer(static_cast<uintptr_t>(candidate), AC0::CHECK_MOON_DIR_Y + 1)) {
        return false;
    }

    __try {
        const float sunIntensity = At<float>(candidate, AC0::SUN_LIGHT_INTENSITY);
        const float moonIntensity = At<float>(candidate, AC0::MOON_LIGHT_INTENSITY);
        const float sunSize = At<float>(candidate, AC0::SUN_SIZE_ANGLE);
        const float sunDirX = At<float>(candidate, AC0::SUN_DIR_X);
        const float sunDirY = At<float>(candidate, AC0::SUN_DIR_Y);
        const float moonSize = At<float>(candidate, AC0::MOON_SIZE_ANGLE);
        const float moonDirX = At<float>(candidate, AC0::MOON_DIR_X);
        const float moonDirY = At<float>(candidate, AC0::MOON_DIR_Y);
        const uint8_t checkSunSize = At<uint8_t>(candidate, AC0::CHECK_SUN_SIZE_ANGLE);
        const uint8_t checkSunX = At<uint8_t>(candidate, AC0::CHECK_SUN_DIR_X);
        const uint8_t checkSunY = At<uint8_t>(candidate, AC0::CHECK_SUN_DIR_Y);
        const uint8_t checkMoonSize = At<uint8_t>(candidate, AC0::CHECK_MOON_SIZE_ANGLE);
        const uint8_t checkMoonX = At<uint8_t>(candidate, AC0::CHECK_MOON_DIR_X);
        const uint8_t checkMoonY = At<uint8_t>(candidate, AC0::CHECK_MOON_DIR_Y);

        if (!InRange(sunIntensity, -200.0f, 200.0f) || !InRange(moonIntensity, -200.0f, 200.0f)) {
            return false;
        }
        if (!InRange(sunSize, 0.0f, 20.0f) || !InRange(moonSize, 0.0f, 20.0f)) {
            return false;
        }
        if (!InRange(sunDirX, -180.0f, 180.0f) || !InRange(sunDirY, -180.0f, 180.0f) ||
            !InRange(moonDirX, -180.0f, 180.0f) || !InRange(moonDirY, -180.0f, 180.0f)) {
            return false;
        }
        if (checkSunSize > 1 || checkSunX > 1 || checkSunY > 1 ||
            checkMoonSize > 1 || checkMoonX > 1 || checkMoonY > 1) {
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

long long ResolveAtmosphereNode(long long atmosphereSource) {
    static long long s_lastLoggedAtmosphereNode = 0;
    if (!atmosphereSource) {
        return 0;
    }

    if (atmosphereSource != s_lastLoggedAtmosphereNode) {
        s_lastLoggedAtmosphereNode = atmosphereSource;
        Log("[atmo] AtmosphereConst0 source = cont+0x20 %p\n", reinterpret_cast<void*>(atmosphereSource));
    }

    return atmosphereSource;
}

} // namespace

ResolvedEnv ResolveEnv() {
    ResolvedEnv r{};
    if (!g_pEnvManager || !*g_pEnvManager) {
        return r;
    }

    __try {
        auto* envMgr = reinterpret_cast<void**>(*g_pEnvManager);
        if (!IsReadablePointer(reinterpret_cast<uintptr_t>(envMgr), sizeof(void*))) {
            return r;
        }

        auto* vtbl = *reinterpret_cast<uintptr_t**>(envMgr);
        if (!IsReadablePointer(reinterpret_cast<uintptr_t>(vtbl), 0x48)) {
            return r;
        }

        using GetEntityFn = long long(__fastcall*)(void*);
        auto getEntity = reinterpret_cast<GetEntityFn>(vtbl[0x40 / 8]);
        if (!getEntity || !IsReadablePointer(reinterpret_cast<uintptr_t>(getEntity), 16)) {
            return r;
        }

        r.entity = getEntity(envMgr);
        if (!r.entity || !IsReadablePointer(static_cast<uintptr_t>(r.entity), 0xEE8)) {
            return r;
        }

        r.weatherState = *reinterpret_cast<long long*>(r.entity + 0xED8);
        if (!r.weatherState || !IsReadablePointer(static_cast<uintptr_t>(r.weatherState), 0x58)) {
            return r;
        }

        long long cont = *reinterpret_cast<long long*>(r.weatherState + 0x50);
        if (!cont || !IsReadablePointer(static_cast<uintptr_t>(cont), 0x28)) {
            return r;
        }

        r.cloudNode = *reinterpret_cast<long long*>(cont + 0x18);
        r.windNode = *reinterpret_cast<long long*>(cont + 0x20);
        if (r.cloudNode && !IsReadablePointer(static_cast<uintptr_t>(r.cloudNode), 0x100)) {
            r.cloudNode = 0;
        }
        if (r.windNode && !IsReadablePointer(static_cast<uintptr_t>(r.windNode), 0x100)) {
            r.windNode = 0;
        }

        r.atmosphereNode = ResolveAtmosphereNode(r.windNode);
        r.particleMgr = *reinterpret_cast<long long*>(r.entity + 0xEE0);
        if (r.particleMgr && !IsReadablePointer(static_cast<uintptr_t>(r.particleMgr), 0x20)) {
            r.particleMgr = 0;
        }

        r.valid = r.entity != 0 && r.weatherState != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = ResolvedEnv{};
    }
    return r;
}

float Clamp01(float v) {
    return min(1.0f, max(0.0f, v));
}

float CloudXToSafeMul(float ui) {
    ui = min(20.0f, max(-20.0f, ui));
    if (ui <= 0.0f) {
        return ((ui + 20.0f) / 20.0f) * 0.25f;
    }
    if (ui <= 1.0f) {
        return 0.25f + 0.75f * ui;
    }
    return 1.0f + ((ui - 1.0f) * (0.50f / 19.0f));
}

float NormalizeHour24(float h) {
    while (h < 0.0f) {
        h += 24.0f;
    }
    while (h >= 24.0f) {
        h -= 24.0f;
    }
    return h;
}

bool ResolveTimeContext(void*& outEnvMgr, long long& outEntity) {
    outEnvMgr = nullptr;
    outEntity = 0;
    if (!g_pEnvManager || !*g_pEnvManager) {
        return false;
    }

    void* envMgr = reinterpret_cast<void*>(*g_pEnvManager);
    auto* vt = *reinterpret_cast<uintptr_t**>(envMgr);
    auto getEntity = reinterpret_cast<long long(__fastcall*)(void*)>(vt[g_tdEnvGetEntity / 8]);
    if (!getEntity) {
        return false;
    }

    long long entity = 0;
    __try {
        entity = getEntity(envMgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (!entity) {
        return false;
    }

    outEnvMgr = envMgr;
    outEntity = entity;
    return true;
}

bool TryReadCurrentTimeRaw(void* envMgr, float& outRaw) {
    if (!envMgr) {
        return false;
    }

    auto* vt = *reinterpret_cast<uintptr_t**>(envMgr);
    auto getTime = reinterpret_cast<EnvGetTimeOfDay_fn>(vt[g_tdEnvGetTime / 8]);
    if (!getTime) {
        return false;
    }

    __try {
        outRaw = static_cast<float>(getTime(envMgr));
        return std::isfinite(outRaw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TrySetTimeRaw(long long entity, float raw) {
    if (!entity || !std::isfinite(raw)) {
        return false;
    }

    auto* vt = *reinterpret_cast<uintptr_t**>(entity);
    auto setTime = reinterpret_cast<EntitySetTimeOfDay_fn>(vt[g_tdEntSetTime / 8]);
    if (!setTime) {
        return false;
    }

    __try {
        setTime(entity, raw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CaptureTimeLimitBaseline(long long entity) {
    if (!entity || g_timeLimitsCaptured.load()) {
        return;
    }

    float lo = 0.0f;
    float hi = 1.0f;
    __try {
        lo = At<float>(entity, g_tdLowerLimit);
        hi = At<float>(entity, g_tdUpperLimit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lo = 0.0f;
        hi = 1.0f;
    }

    if (!std::isfinite(lo) || !std::isfinite(hi)) {
        lo = 0.0f;
        hi = 1.0f;
    }

    g_timeBaseLower.store(lo);
    g_timeBaseUpper.store(hi);
    g_timeDomainHours.store(hi > 1.5f && hi <= 48.0f);
    g_timeDomainKnown.store(true);
    g_timeLimitsCaptured.store(true);
    Log("[visual-time] baseline lower=%.4f upper=%.4f domain=%s\n",
        lo,
        hi,
        g_timeDomainHours.load() ? "hours" : "normalized");
}

void RestoreTimeLimitBaseline(long long entity) {
    if (!entity || !g_timeLimitsCaptured.load()) {
        return;
    }

    __try {
        At<float>(entity, g_tdLowerLimit) = g_timeBaseLower.load();
        At<float>(entity, g_tdUpperLimit) = g_timeBaseUpper.load();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[W] visual-time restore baseline exception\n");
    }
}

float UIHourToEngineRaw(float hour) {
    hour = NormalizeHour24(hour);
    return g_timeDomainKnown.load() && !g_timeDomainHours.load() ? (hour / 24.0f) : hour;
}

bool TryReadCurrentHourFromEntity(long long entity, float& outHour) {
    if (!entity) {
        return false;
    }

    float lo = g_timeBaseLower.load();
    float hi = g_timeBaseUpper.load();
    float a = 0.0f;
    float b = 0.0f;
    __try {
        a = At<float>(entity, g_tdCurrentA);
        b = At<float>(entity, g_tdCurrentB);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    auto inRange = [&](float raw) {
        return std::isfinite(raw) && raw >= (lo - 0.5f) && raw <= (hi + 0.5f);
    };

    float raw = NAN;
    if (inRange(a) && !inRange(b)) {
        raw = a;
    } else if (inRange(b) && !inRange(a)) {
        raw = b;
    } else if (inRange(a)) {
        raw = a;
    }

    if (!std::isfinite(raw)) {
        return false;
    }

    outHour = NormalizeHour24(g_timeDomainHours.load() ? raw : raw * 24.0f);
    return true;
}

void TickTimeControl() {
    if (!g_timeLayoutReady.load()) {
        return;
    }

    void* envMgr = nullptr;
    long long entity = 0;
    if (!ResolveTimeContext(envMgr, entity)) {
        return;
    }

    CaptureTimeLimitBaseline(entity);

    float currentHour = NAN;
    if (TryReadCurrentHourFromEntity(entity, currentHour)) {
        g_timeCurrentHour.store(currentHour);
    } else {
        float currentRaw = 0.0f;
        if (TryReadCurrentTimeRaw(envMgr, currentRaw)) {
            g_timeCurrentHour.store(NormalizeHour24(g_timeDomainHours.load() ? currentRaw : (currentRaw * 24.0f)));
        }
    }

    const bool active = g_timeCtrlActive.load();
    const bool freeze = g_timeFreeze.load();
    if (!active && !freeze) {
        g_timeOriginalHour.store(g_timeCurrentHour.load());
        g_timeOriginalHourValid.store(true);
    }
    if (!active && !freeze) {
        if (g_timeFreezeApplied.exchange(false)) {
            RestoreTimeLimitBaseline(entity);
        }
        g_timeSetHoldTicks.store(0);
        return;
    }

    const float targetHour = NormalizeHour24(g_timeTargetHour.load());
    const float targetRaw = UIHourToEngineRaw(targetHour);
    const bool applyNow = g_timeApplyRequest.exchange(false);

    if (freeze) {
        const float frozenRaw = g_timeFrozenRaw.load();
        const bool needApply =
            !g_timeFreezeApplied.load() || applyNow || fabsf(frozenRaw - targetRaw) > 0.001f;
        if (needApply) {
            TrySetTimeRaw(entity, targetRaw);
            __try {
                At<float>(entity, g_tdLowerLimit) = targetRaw;
                At<float>(entity, g_tdUpperLimit) = targetRaw;
                g_timeFreezeApplied.store(true);
                g_timeFrozenRaw.store(targetRaw);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("[W] visual-time freeze write exception\n");
            }
        }
        g_timeSetHoldTicks.store(0);
        return;
    }

    if (g_timeFreezeApplied.exchange(false)) {
        RestoreTimeLimitBaseline(entity);
    }
    if (applyNow) {
        g_timeSetHoldTicks.store(8);
    }

    const int holdTicks = g_timeSetHoldTicks.load();
    if (holdTicks > 0) {
        TrySetTimeRaw(entity, targetRaw);
        g_timeSetHoldTicks.store(holdTicks - 1);
    }
}

void SuspendTimeControl() {
    if (!g_timeLayoutReady.load()) {
        return;
    }

    void* envMgr = nullptr;
    long long entity = 0;
    if (!ResolveTimeContext(envMgr, entity)) {
        return;
    }

    CaptureTimeLimitBaseline(entity);
    if (g_timeFreezeApplied.exchange(false)) {
        RestoreTimeLimitBaseline(entity);
        Log("[visual-time] suspended, baseline restored\n");
    }
    g_timeSetHoldTicks.store(0);
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
    return g_pNativeToastCreateString && g_pNativeToastPush && g_pNativeToastReleaseString &&
           g_pNativeToastRootGlobal && g_nativeToastOuterOffset != 0 && g_nativeToastManagerOffset != 0;
}

void ShowNativeToast(const char* msg) {
    if (!msg || !msg[0] || !NativeToastReady()) {
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
