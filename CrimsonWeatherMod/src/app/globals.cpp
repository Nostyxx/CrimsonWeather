#include "pch.h"
#include "cw_shared.h"

namespace {

void WriteDefaultConfig(const char* path) {
    if (!path || !path[0]) return;
    WritePrivateProfileStringA("General","LogEnabled","1",path);
    WritePrivateProfileStringA("General","HotkeyToggleGUI","F9",path);
    WritePrivateProfileStringA("General","HotkeyToggleEffect","F10",path);
    WritePrivateProfileStringA("General","_HotkeyOptions",
        "F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, or single letter A-Z",path);
    WritePrivateProfileStringA("Hotkeys","ControllerHotkeyToggleGUI","dpad_down+b",path);
    WritePrivateProfileStringA("Hotkeys","ControllerToggleEffect","dpad_down+x",path);
    WritePrivateProfileStringA("Hotkeys","_ControllerHotkeyOptions",
        "Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back",path);
    WritePrivateProfileStringA("UI","Scale","1.00",path);
    WritePrivateProfileStringA("UI", "ShowOnStartup", "1", path);
}

void PatchMissingConfigKeys(const char* path) {
    if (!path || !path[0]) return;
    char buf[64] = {};

    if (GetPrivateProfileStringA("General","HotkeyToggleEffect","",buf,sizeof(buf),path) == 0) {
        WritePrivateProfileStringA("General","HotkeyToggleEffect","F10",path);
    }
    if (GetPrivateProfileStringA("Hotkeys","ControllerHotkeyToggleGUI","",buf,sizeof(buf),path) == 0) {
        WritePrivateProfileStringA("Hotkeys","ControllerHotkeyToggleGUI","dpad_down+b",path);
    }
    if (GetPrivateProfileStringA("Hotkeys","ControllerToggleEffect","",buf,sizeof(buf),path) == 0) {
        WritePrivateProfileStringA("Hotkeys","ControllerToggleEffect","dpad_down+x",path);
    }
    if (GetPrivateProfileStringA("Hotkeys","_ControllerHotkeyOptions","",buf,sizeof(buf),path) == 0) {
        WritePrivateProfileStringA("Hotkeys","_ControllerHotkeyOptions",
            "Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back",path);
    }
    if (GetPrivateProfileStringA("UI", "ShowOnStartup", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("UI", "ShowOnStartup", "1", path);
    }

    GetPrivateProfileStringA("Preset", "LastPreset", "", buf, sizeof(buf), path);
    WritePrivateProfileStringA("Preset", "LastPreset", buf, path);
}

} // namespace

void Log(const char* fmt, ...) {
    if (!g_logEnabled || !g_logFile) return;
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    fprintf(g_logFile,"[%02d:%02d:%02d.%03d] ",st.wHour,st.wMinute,st.wSecond,st.wMilliseconds);
    va_list a;
    va_start(a, fmt);
    vfprintf(g_logFile, fmt, a);
    va_end(a);
    fflush(g_logFile);
}

const char* RuntimeHealthStateLabel(RuntimeHealthState state) {
    switch (state) {
    case RuntimeHealthState::Ready: return "READY";
    case RuntimeHealthState::Degraded: return "DEGRADED";
    default: return "DISABLED";
    }
}

const char* AobTargetLabel(AobTargetId id) {
    switch (id) {
    case AobTargetId::WeatherTick: return "WeatherTick";
    case AobTargetId::GetRainIntensity: return "GetRainIntensity";
    case AobTargetId::GetSnowIntensity: return "GetSnowIntensity";
    case AobTargetId::GetDustIntensity: return "GetDustIntensity";
    case AobTargetId::ProcessWindState: return "ProcessWindState";
    case AobTargetId::ActivateEffect: return "ActivateEffect";
    case AobTargetId::SetIntensity: return "SetIntensity";
    case AobTargetId::CloudPack: return "CloudPack";
    case AobTargetId::WindPack: return "WindPack";
    case AobTargetId::PostProcessLayerUpdate: return "PostProcessLayerUpdate";
    case AobTargetId::GetLayerMeta: return "GetLayerMeta";
    case AobTargetId::WeatherFrameUpdate: return "WeatherFrameUpdate";
    case AobTargetId::AtmosFogBlend: return "AtmosFogBlend";
    case AobTargetId::EnvManagerPtr: return "g_EnvManagerPtr";
    case AobTargetId::NullSentinel: return "g_NullSentinel";
    case AobTargetId::TimeStores: return "TimeStores";
    case AobTargetId::TimeDebugHandler: return "TimeDebugHandler";
    case AobTargetId::NativeToast: return "NativeToast";
    default: return "UnknownTarget";
    }
}

const char* RuntimeHealthGroupLabel(RuntimeHealthGroup id) {
    switch (id) {
    case RuntimeHealthGroup::CoreWeather: return "CoreWeather";
    case RuntimeHealthGroup::CloudExperiment: return "CloudExperiment";
    case RuntimeHealthGroup::Fog: return "Fog";
    case RuntimeHealthGroup::Time: return "Time";
    case RuntimeHealthGroup::Infra: return "Infra";
    default: return "UnknownGroup";
    }
}

const char* RuntimeFeatureLabel(RuntimeFeatureId id) {
    switch (id) {
    case RuntimeFeatureId::ForceClear: return "ForceClear";
    case RuntimeFeatureId::Rain: return "Rain";
    case RuntimeFeatureId::Dust: return "Dust";
    case RuntimeFeatureId::Snow: return "Snow";
    case RuntimeFeatureId::TimeControls: return "TimeControls";
    case RuntimeFeatureId::CloudControls: return "CloudControls";
    case RuntimeFeatureId::FogControls: return "FogControls";
    case RuntimeFeatureId::WindControls: return "WindControls";
    case RuntimeFeatureId::NoWindControls: return "NoWindControls";
    case RuntimeFeatureId::DetailControls: return "DetailControls";
    case RuntimeFeatureId::ExperimentControls: return "ExperimentControls";
    case RuntimeFeatureId::NativeToast: return "NativeToast";
    default: return "UnknownFeature";
    }
}

void ClearRuntimeHealthState() {
    for (auto& entry : g_aobTargetHealth) entry = RuntimeHealthEntry{};
    for (auto& entry : g_runtimeGroupHealth) entry = RuntimeHealthEntry{};
    for (auto& entry : g_runtimeFeatureHealth) entry = RuntimeHealthEntry{};
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
    if (!outPath || outSize == 0) return;
    if (g_pluginDir[0]) sprintf_s(outPath, outSize, "%s\\CrimsonWeather.ini", g_pluginDir);
    else strcpy_s(outPath, outSize, "CrimsonWeather.ini");
}

int KeyNameToVK(const char* n) {
    if(!_stricmp(n,"F1")) return VK_F1;  if(!_stricmp(n,"F2")) return VK_F2;
    if(!_stricmp(n,"F3")) return VK_F3;  if(!_stricmp(n,"F4")) return VK_F4;
    if(!_stricmp(n,"F5")) return VK_F5;  if(!_stricmp(n,"F6")) return VK_F6;
    if(!_stricmp(n,"F7")) return VK_F7;  if(!_stricmp(n,"F8")) return VK_F8;
    if(!_stricmp(n,"F9")) return VK_F9;  if(!_stricmp(n,"F10"))return VK_F10;
    if(!_stricmp(n,"F11"))return VK_F11; if(!_stricmp(n,"F12"))return VK_F12;
    if(!_stricmp(n,"INSERT"))return VK_INSERT; if(!_stricmp(n,"DELETE"))return VK_DELETE;
    if(!_stricmp(n,"HOME"))return VK_HOME; if(!_stricmp(n,"END"))return VK_END;
    if(!_stricmp(n,"PGUP"))return VK_PRIOR; if(!_stricmp(n,"PGDN"))return VK_NEXT;
    if(strlen(n)==1)return toupper(n[0]);
    return VK_F9;
}

std::string VKToKeyName(int vk) {
    if(vk==VK_F1)  return "F1";  if(vk==VK_F2)  return "F2";
    if(vk==VK_F3)  return "F3";  if(vk==VK_F4)  return "F4";
    if(vk==VK_F5)  return "F5";  if(vk==VK_F6)  return "F6";
    if(vk==VK_F7)  return "F7";  if(vk==VK_F8)  return "F8";
    if(vk==VK_F9)  return "F9";  if(vk==VK_F10) return "F10";
    if(vk==VK_F11) return "F11"; if(vk==VK_F12) return "F12";
    if(vk==VK_INSERT) return "INSERT"; if(vk==VK_DELETE) return "DELETE";
    if(vk==VK_HOME)   return "HOME";   if(vk==VK_END)    return "END";
    if(vk==VK_PRIOR)  return "PGUP";   if(vk==VK_NEXT)   return "PGDN";
    return std::string(1, static_cast<char>(vk));
}

WORD ControllerTokenToMask(const char* token) {
    if (!token || !token[0]) return 0;
    if (!_stricmp(token, "dpad_up") || !_stricmp(token, "up")) return XINPUT_GAMEPAD_DPAD_UP;
    if (!_stricmp(token, "dpad_down") || !_stricmp(token, "down")) return XINPUT_GAMEPAD_DPAD_DOWN;
    if (!_stricmp(token, "dpad_left") || !_stricmp(token, "left")) return XINPUT_GAMEPAD_DPAD_LEFT;
    if (!_stricmp(token, "dpad_right") || !_stricmp(token, "right")) return XINPUT_GAMEPAD_DPAD_RIGHT;
    if (!_stricmp(token, "a") || !_stricmp(token, "cross")) return XINPUT_GAMEPAD_A;
    if (!_stricmp(token, "b") || !_stricmp(token, "circle")) return XINPUT_GAMEPAD_B;
    if (!_stricmp(token, "x") || !_stricmp(token, "square")) return XINPUT_GAMEPAD_X;
    if (!_stricmp(token, "y") || !_stricmp(token, "triangle")) return XINPUT_GAMEPAD_Y;
    if (!_stricmp(token, "lb") || !_stricmp(token, "l1")) return XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (!_stricmp(token, "rb") || !_stricmp(token, "r1")) return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (!_stricmp(token, "start") || !_stricmp(token, "options")) return XINPUT_GAMEPAD_START;
    if (!_stricmp(token, "back") || !_stricmp(token, "select") || !_stricmp(token, "share")) return XINPUT_GAMEPAD_BACK;
    return 0;
}

WORD ParseControllerCombo(const char* text, WORD fallback) {
    if (!text || !text[0]) return fallback;
    char copy[128] = {};
    strncpy_s(copy, text, _TRUNCATE);
    WORD mask = 0;
    char* context = nullptr;
    for (char* token = strtok_s(copy, "+|, ", &context); token; token = strtok_s(nullptr, "+|, ", &context)) {
        mask = WORD(mask | ControllerTokenToMask(token));
    }
    return mask ? mask : fallback;
}

bool IsControllerComboPressed(WORD buttons, WORD comboMask) {
    return comboMask != 0 && (buttons & comboMask) == comboMask;
}

void ControllerComboToDisplayString(WORD mask, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (mask == 0) {
        strcpy_s(out, outSize, "<unbound>");
        return;
    }

    struct PartDesc { WORD mask; const char* label; };
    static constexpr PartDesc kParts[] = {
        { XINPUT_GAMEPAD_DPAD_UP, "D-pad Up" },
        { XINPUT_GAMEPAD_DPAD_DOWN, "D-pad Down" },
        { XINPUT_GAMEPAD_DPAD_LEFT, "D-pad Left" },
        { XINPUT_GAMEPAD_DPAD_RIGHT, "D-pad Right" },
        { XINPUT_GAMEPAD_A, "A" },
        { XINPUT_GAMEPAD_B, "B" },
        { XINPUT_GAMEPAD_X, "X" },
        { XINPUT_GAMEPAD_Y, "Y" },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, "LB" },
        { XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB" },
        { XINPUT_GAMEPAD_START, "Start" },
        { XINPUT_GAMEPAD_BACK, "Back" },
    };

    bool first = true;
    for (const PartDesc& part : kParts) {
        if ((mask & part.mask) == 0) continue;
        if (!first) strcat_s(out, outSize, " + ");
        strcat_s(out, outSize, part.label);
        first = false;
    }
    if (first) strcpy_s(out, outSize, "<unbound>");
}

void LoadConfig(const char* dir) {
    if (dir && dir[0]) strcpy_s(g_pluginDir, dir);
    char path[MAX_PATH] = {};
    BuildIniPath(path, sizeof(path));
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        WriteDefaultConfig(path);
    } else {
        PatchMissingConfigKeys(path);
    }

    char buf[64] = {};
    GetPrivateProfileStringA("General","LogEnabled","1",buf,sizeof(buf),path);
    g_cfg.logEnabled = atoi(buf) != 0;
    GetPrivateProfileStringA("General","HotkeyToggleGUI","F9",buf,sizeof(buf),path);
    g_cfg.hotkeyVK = KeyNameToVK(buf);
    GetPrivateProfileStringA("General","HotkeyToggleEffect","F10",buf,sizeof(buf),path);
    g_cfg.effectToggleVK = KeyNameToVK(buf);
    GetPrivateProfileStringA("Hotkeys","ControllerHotkeyToggleGUI","dpad_down+b",buf,sizeof(buf),path);
    g_cfg.controllerGuiToggleMask = ParseControllerCombo(buf, WORD(XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_B));
    GetPrivateProfileStringA("Hotkeys","ControllerToggleEffect","dpad_down+x",buf,sizeof(buf),path);
    g_cfg.controllerEffectToggleMask = ParseControllerCombo(buf, WORD(XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_X));
    GetPrivateProfileStringA("UI","Scale","1.00",buf,sizeof(buf),path);
    g_cfg.uiScale = ClampUiScale((float)atof(buf));
    GetPrivateProfileStringA("UI", "ShowOnStartup", "1", buf, sizeof(buf), path);
    g_cfg.showGuiOnStartup = atoi(buf) != 0;
}

void SaveConfigUIScale() {
    char path[MAX_PATH] = {};
    BuildIniPath(path, sizeof(path));
    char buf[32] = {};
    float s = ClampUiScale(g_cfg.uiScale);
    sprintf_s(buf, "%.2f", s);
    WritePrivateProfileStringA("UI", "Scale", buf, path);
}

void SaveConfigShowGuiOnStartup() {
    char path[MAX_PATH] = {};
    BuildIniPath(path, sizeof(path));
    WritePrivateProfileStringA("UI", "ShowOnStartup", g_cfg.showGuiOnStartup ? "1" : "0", path);
}

void OpenLogFile(const char* dir) {
    if(!g_cfg.logEnabled){g_logEnabled=false;return;}
    char path[MAX_PATH]; sprintf_s(path,"%s\\CrimsonWeather.log",dir);
    fopen_s(&g_logFile,path,"w");
    if(!g_logFile)g_logEnabled=false;
}

void GUI_SetStatus(const char* msg) {
    if (!msg) return;
    strncpy_s(g_statusText, msg, _TRUNCATE);
}

void ApplyUiScale(float scale) {
    if (!ImGui::GetCurrentContext()) return;
    scale = ClampUiScale(scale);
    ImGuiStyle& style = ImGui::GetStyle();
    if (!g_imguiBaseStyleValid) {
        g_imguiBaseStyle = style;
        g_imguiBaseStyleValid = true;
    }
    style = g_imguiBaseStyle;
    style.ScaleAllSizes(scale);
    ImGuiIO& io = ImGui::GetIO();
    io.FontGlobalScale = scale;
    g_uiScaleApplied = scale;
}

void SyncMenuCursorState() {
    if (!g_imguiReady) return;
    static int s_cursorRefDelta = 0;
    ImGuiIO& io = ImGui::GetIO();
    io.MouseDrawCursor = g_menuOpen;
    if (g_menuOpen) {
        for (int i = 0; i < 16; ++i) {
            CURSORINFO ci{};
            ci.cbSize = sizeof(ci);
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) break;
            if (s_cursorRefDelta >= 64) break;
            ShowCursor(TRUE);
            ++s_cursorRefDelta;
        }
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
        if (g_pOrigClipCursor) g_pOrigClipCursor(nullptr);
    } else {
        while (s_cursorRefDelta > 0) {
            ShowCursor(FALSE);
            --s_cursorRefDelta;
        }
    }
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
    g_noWind.store(false);
    g_windMul.store(1.0f);
    g_timeCtrlActive.store(false);
    g_timeFreeze.store(false);
    g_timeApplyRequest.store(false);
    g_timeTargetHour.store(g_timeCurrentHour.load());
    g_timeSetHoldTicks.store(0);
    g_timeFrozenRaw.store(-9999.0f);
    g_cloudBaseValid.store(false);
    g_windPackBaseValid.store(false);
    g_resetStopRequested.store(true);
}

bool AnySliderActive() {
    return g_oRain.active.load() ||
           g_oSnow.active.load() || g_oDust.active.load() ||
           g_oFog.active.load() ||
           g_oCloudSpdX.active.load() || g_oCloudSpdY.active.load() ||
           g_oHighClouds.active.load() ||
           g_oAtmoAlpha.active.load() ||
           g_oExpCloud2C.active.load() ||
           g_oExpCloud2D.active.load() ||
           g_oExpNightSkyRot.active.load() ||
           g_oCloudThk.active.load() ||
           g_oWind.active.load();
}

bool AnyCustomWeatherSliderActive() {
    return g_oRain.active.load() ||
           g_oSnow.active.load() || g_oDust.active.load() ||
           g_oFog.active.load() ||
           g_oCloudSpdX.active.load() || g_oCloudSpdY.active.load() ||
           g_oHighClouds.active.load() ||
           g_oAtmoAlpha.active.load() ||
           g_oCloudThk.active.load() ||
           g_oWind.active.load();
}

ResolvedEnv ResolveEnv() {
    ResolvedEnv r = {};
    if (!g_pEnvManager || !*g_pEnvManager) return r;
    auto* envMgr = reinterpret_cast<void**>(*g_pEnvManager);
    auto* vtbl = *reinterpret_cast<uintptr_t**>(envMgr);
    using Fn = long long(__fastcall*)(void*);
    r.entity = reinterpret_cast<Fn>(vtbl[0x40 / 8])(envMgr);
    if (!r.entity) return r;
    r.weatherState = *reinterpret_cast<long long*>(r.entity + 0xED8);
    if (!r.weatherState) return r;
    long long cont = *reinterpret_cast<long long*>(r.weatherState + 0x50);
    if (!cont) return r;
    r.cloudNode = *reinterpret_cast<long long*>(cont + 0x18);
    r.windNode = *reinterpret_cast<long long*>(cont + 0x20);
    r.particleMgr = *reinterpret_cast<long long*>(r.entity + 0xEE0);
    r.valid = true;
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
    while (h < 0.0f) h += 24.0f;
    while (h >= 24.0f) h -= 24.0f;
    return h;
}

bool ResolveTimeContext(void*& outEnvMgr, long long& outEntity) {
    outEnvMgr = nullptr;
    outEntity = 0;
    if (!g_pEnvManager || !*g_pEnvManager) return false;
    void* envMgr = reinterpret_cast<void*>(*g_pEnvManager);
    if (!envMgr) return false;
    auto* vt = *reinterpret_cast<uintptr_t**>(envMgr);
    if (!vt) return false;
    auto getEntity = reinterpret_cast<long long(__fastcall*)(void*)>(vt[g_tdEnvGetEntity / 8]);
    if (!getEntity) return false;
    long long ent = 0;
    __try {
        ent = getEntity(envMgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!ent) return false;
    outEnvMgr = envMgr;
    outEntity = ent;
    return true;
}

bool TryReadCurrentTimeRaw(void* envMgr, float& outRaw) {
    if (!envMgr) return false;
    auto* vt = *reinterpret_cast<uintptr_t**>(envMgr);
    if (!vt) return false;
    auto getTime = reinterpret_cast<EnvGetTimeOfDay_fn>(vt[g_tdEnvGetTime / 8]);
    if (!getTime) return false;
    __try {
        double raw = getTime(envMgr);
        outRaw = static_cast<float>(raw);
        return std::isfinite(outRaw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TrySetTimeRaw(long long entity, float raw) {
    if (!entity || !std::isfinite(raw)) return false;
    auto* vt = *reinterpret_cast<uintptr_t**>(entity);
    if (!vt) return false;
    auto setTime = reinterpret_cast<EntitySetTimeOfDay_fn>(vt[g_tdEntSetTime / 8]);
    if (!setTime) return false;
    __try {
        setTime(entity, raw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CaptureTimeLimitBaseline(long long entity) {
    if (!entity || g_timeLimitsCaptured.load()) return;
    float lo = 0.0f, hi = 1.0f;
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
    bool hoursDomain = (hi > 1.5f && hi <= 48.0f);
    g_timeDomainHours.store(hoursDomain);
    g_timeDomainKnown.store(true);
    g_timeLimitsCaptured.store(true);
    Log("[visual-time] baseline limits lower=%.4f upper=%.4f domain=%s\n",
        lo, hi, hoursDomain ? "hours" : "normalized");
}

void RestoreTimeLimitBaseline(long long entity) {
    if (!entity || !g_timeLimitsCaptured.load()) return;
    float lo = g_timeBaseLower.load();
    float hi = g_timeBaseUpper.load();
    __try {
        At<float>(entity, g_tdLowerLimit) = lo;
        At<float>(entity, g_tdUpperLimit) = hi;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[W] visual-time restore baseline exception\n");
    }
}

float UIHourToEngineRaw(float hour) {
    hour = NormalizeHour24(hour);
    bool domainHours = g_timeDomainKnown.load() ? g_timeDomainHours.load() : true;
    return domainHours ? hour : (hour / 24.0f);
}

bool TryReadCurrentHourFromEntity(long long entity, float& outHour) {
    if (!entity) return false;
    float lo = g_timeBaseLower.load();
    float hi = g_timeBaseUpper.load();
    float a = 0.0f, b = 0.0f;
    __try {
        a = At<float>(entity, g_tdCurrentA);
        b = At<float>(entity, g_tdCurrentB);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    auto toHour = [&](float raw) -> float {
        if (!std::isfinite(raw)) return NAN;
        if (g_timeDomainHours.load()) return raw;
        return raw * 24.0f;
    };
    auto inRangeRaw = [&](float raw) -> bool {
        return std::isfinite(raw) && raw >= (lo - 0.5f) && raw <= (hi + 0.5f);
    };
    float pick = NAN;
    if (inRangeRaw(a) && !inRangeRaw(b)) pick = a;
    else if (inRangeRaw(b) && !inRangeRaw(a)) pick = b;
    else if (inRangeRaw(a)) pick = a;
    if (!std::isfinite(pick)) return false;
    float h = toHour(pick);
    if (!std::isfinite(h)) return false;
    outHour = NormalizeHour24(h);
    return true;
}

void TickTimeControl() {
    if (!g_timeLayoutReady.load()) return;
    void* envMgr = nullptr;
    long long entity = 0;
    if (!ResolveTimeContext(envMgr, entity)) return;

    CaptureTimeLimitBaseline(entity);

    float curHour = NAN;
    if (TryReadCurrentHourFromEntity(entity, curHour)) {
        g_timeCurrentHour.store(curHour);
    } else {
        float curRaw = 0.0f;
        if (TryReadCurrentTimeRaw(envMgr, curRaw)) {
            float h = g_timeDomainHours.load() ? curRaw : (curRaw * 24.0f);
            if (std::isfinite(h)) g_timeCurrentHour.store(NormalizeHour24(h));
        }
    }

    bool active = g_timeCtrlActive.load();
    bool freeze = g_timeFreeze.load();

    if (!active && !freeze) {
        if (g_timeFreezeApplied.exchange(false)) {
            RestoreTimeLimitBaseline(entity);
            Log("[visual-time] freeze released, baseline limits restored\n");
        }
        g_timeSetHoldTicks.store(0);
        return;
    }

    float targetHour = NormalizeHour24(g_timeTargetHour.load());
    float targetRaw = UIHourToEngineRaw(targetHour);
    bool applyNow = g_timeApplyRequest.exchange(false);

    if (freeze) {
        float prevFrozen = g_timeFrozenRaw.load();
        bool needApply = !g_timeFreezeApplied.load() || applyNow || fabsf(prevFrozen - targetRaw) > 0.001f;
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
    } else {
        if (g_timeFreezeApplied.exchange(false)) {
            RestoreTimeLimitBaseline(entity);
        }
        if (applyNow) {
            g_timeSetHoldTicks.store(8);
        }
        int hold = g_timeSetHoldTicks.load();
        if (hold > 0) {
            TrySetTimeRaw(entity, targetRaw);
            if (hold > 0) g_timeSetHoldTicks.store(hold - 1);
        }
    }
}

void SuspendTimeControl() {
    if (!g_timeLayoutReady.load()) return;
    void* envMgr = nullptr;
    long long entity = 0;
    if (!ResolveTimeContext(envMgr, entity)) return;
    CaptureTimeLimitBaseline(entity);
    if (g_timeFreezeApplied.exchange(false)) {
        RestoreTimeLimitBaseline(entity);
        Log("[visual-time] suspended, baseline limits restored\n");
    }
    g_timeSetHoldTicks.store(0);
}

void SetModEnabled(bool enabled) {
    const bool wasEnabled = g_modEnabled.exchange(enabled);
    if (wasEnabled == enabled) return;
    if (!enabled) {
        g_modSuspendRequested.store(true);
        GUI_SetStatus("Weather control disabled");
        Log("[i] Weather control disabled\n");
        ShowNativeToast("CRIMSON WEATHER DISABLED");
    } else {
        GUI_SetStatus("Weather control enabled");
        Log("[i] Weather control enabled\n");
        ShowNativeToast("CRIMSON WEATHER ENABLED");
    }
}

void ToggleModEnabled() {
    SetModEnabled(!g_modEnabled.load());
}

void* ResolveNativeToastManager() {
    if (!g_pNativeToastRootGlobal) return nullptr;
    void* root = *g_pNativeToastRootGlobal;
    if (!root || g_nativeToastOuterOffset == 0 || g_nativeToastManagerOffset == 0) return nullptr;
    __try {
        void* outer = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(root) + g_nativeToastOuterOffset);
        if (!outer) return nullptr;
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(outer) + g_nativeToastManagerOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool NativeToastReady() {
    return g_pNativeToastCreateString &&
           g_pNativeToastPush &&
           g_pNativeToastReleaseString &&
           g_pNativeToastRootGlobal &&
           g_nativeToastOuterOffset != 0 &&
           g_nativeToastManagerOffset != 0;
}

void ShowNativeToast(const char* msg) {
    if (!msg || !msg[0]) return;
    if (!NativeToastReady()) return;

    void* manager = ResolveNativeToastManager();
    if (!manager) {
        Log("[W] Native toast manager unavailable for message: %s\n", msg);
        return;
    }

    void* messageHandle = g_pNativeToastCreateString(msg);
    if (!messageHandle) {
        Log("[W] Failed to create native toast string: %s\n", msg);
        return;
    }

    g_pNativeToastPush(manager, &messageHandle, 0);
    g_pNativeToastReleaseString(messageHandle);
    Log("[i] Native toast shown: %s\n", msg);
}
