#pragma once

#include "MinHook.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

using std::max;
using std::min;

#if defined(CW_WIND_ONLY)
#define MOD_NAME "Crimson Weather"
#define MOD_DISPLAY_NAME "Crimson Weather (Wind only)"
#define MOD_CONFIG_FILE "CrimsonWeather.WindOnly.ini"
#define MOD_LOG_FILE "CrimsonWeather.WindOnly.log"
#else
#define MOD_NAME "Crimson Weather"
#define MOD_DISPLAY_NAME MOD_NAME
#define MOD_CONFIG_FILE "CrimsonWeather.ini"
#define MOD_LOG_FILE "CrimsonWeather.log"
#endif

#define MOD_VERSION "0.4.2"

struct Config {
    bool logEnabled = true;
    int effectToggleVK = VK_F10;
    WORD controllerEffectToggleMask = 0;
    float uiScale = 1.0f;
    bool reshadeDiagnostics = false;
};

inline Config g_cfg{};
inline FILE* g_logFile = nullptr;
inline bool g_logEnabled = true;
inline char g_pluginDir[MAX_PATH] = {};
inline constexpr float kUiScaleMin = 0.75f;
inline constexpr float kUiScaleMax = 3.00f;

void Log(const char* fmt, ...);
float ClampUiScale(float v);
void BuildIniPath(char* outPath, size_t outSize);
int KeyNameToVK(const char* name);
WORD ControllerTokenToMask(const char* token);
WORD ParseControllerCombo(const char* text, WORD fallback);
bool IsControllerComboPressed(WORD buttons, WORD comboMask);
void LoadConfig(const char* dir);
void SaveConfigUIScale();
void SaveWindOnlyConfig();
void OpenLogFile(const char* dir);
void GUI_SetStatus(const char* msg);
void ApplyUiScale(float scale);
void ResetAllSliders();
bool AnySliderActive();
bool AnyCustomWeatherSliderActive();

constexpr bool IsWindOnlyBuild() {
#if defined(CW_WIND_ONLY)
    return true;
#else
    return false;
#endif
}

namespace WCO {
    constexpr ptrdiff_t WIND_EVENT_A = 0xD0;
    constexpr ptrdiff_t WIND_EVENT_B = 0xD4;
    constexpr ptrdiff_t LERP_ALPHA = 0xEC;
    constexpr ptrdiff_t BLEND_DIR_MULT = 0xF0;
    constexpr ptrdiff_t HANDLE_ARRAY = 0xF4;
    constexpr ptrdiff_t SOUND_RAIN = 0xD8;
    constexpr ptrdiff_t SOUND_WIND = 0xDC;
    constexpr ptrdiff_t SOUND_SKYWIND = 0xE0;
}

namespace CN {
    constexpr ptrdiff_t DUST_BASE = 0x120;
    constexpr ptrdiff_t DUST_ADD = 0x18C;
    constexpr ptrdiff_t DUST_WIND_SCALE = 0x140;
    constexpr ptrdiff_t FOG_A = 0x11C;
    constexpr ptrdiff_t CLOUD_TOP = 0x124;
    constexpr ptrdiff_t CLOUD_THICK = 0x128;
    constexpr ptrdiff_t CLOUD_BASE = 0x12C;
    constexpr ptrdiff_t CLOUD_SHAPE_A = 0x130;
    constexpr ptrdiff_t CLOUD_SHAPE_B = 0x134;
    constexpr ptrdiff_t CLOUD_SHAPE_C = 0x138;
    constexpr ptrdiff_t DUST_THRESH = 0x180;
    constexpr ptrdiff_t STORM_THRESH = 0x184;
    constexpr ptrdiff_t FOG_B = 0x188;
}

namespace AC0 {
    constexpr ptrdiff_t SUN_LIGHT_INTENSITY = 0x40;
    constexpr ptrdiff_t MOON_LIGHT_INTENSITY = 0x48;
    constexpr ptrdiff_t SUN_SIZE_ANGLE = 0x88;
    constexpr ptrdiff_t SUN_DIR_X = 0x8C;
    constexpr ptrdiff_t SUN_DIR_Y = 0x90;
    constexpr ptrdiff_t MOON_SIZE_ANGLE = 0x94;
    constexpr ptrdiff_t MOON_DIR_X = 0x98;
    constexpr ptrdiff_t MOON_DIR_Y = 0x9C;
    constexpr ptrdiff_t CHECK_SUN_SIZE_ANGLE = 0x11D;
    constexpr ptrdiff_t CHECK_SUN_DIR_X = 0x11E;
    constexpr ptrdiff_t CHECK_SUN_DIR_Y = 0x11F;
    constexpr ptrdiff_t CHECK_MOON_SIZE_ANGLE = 0x120;
    constexpr ptrdiff_t CHECK_MOON_DIR_X = 0x121;
    constexpr ptrdiff_t CHECK_MOON_DIR_Y = 0x122;
}

namespace WN {
    constexpr ptrdiff_t DIR_X = 0x18;
    constexpr ptrdiff_t DIR_Z = 0x1C;
    constexpr ptrdiff_t TURB_DENS = 0x60;
    constexpr ptrdiff_t TURB_SCALE = 0x64;
    constexpr ptrdiff_t TURB_LIFT = 0x68;
    constexpr ptrdiff_t SPEED = 0x88;
    constexpr ptrdiff_t GUST = 0x9C;
    constexpr ptrdiff_t SNOW_GROUND = 0xA8;
    constexpr ptrdiff_t CLOTH_WIND_X = 0xB8;
    constexpr ptrdiff_t CLOTH_WIND_Z = 0xBC;
    constexpr ptrdiff_t CLOUD_SCROLL_X = 0xD0;
    constexpr ptrdiff_t CLOUD_SCROLL_Z = 0xD4;
}

namespace TD {
    constexpr ptrdiff_t LOWER_LIMIT_DEF = 0x3CC;
    constexpr ptrdiff_t UPPER_LIMIT_DEF = 0x3D0;
    constexpr ptrdiff_t CURRENT_A_DEF = 0x3C8;
    constexpr ptrdiff_t CURRENT_B_DEF = 0x3C4;
    constexpr ptrdiff_t ENV_GET_ENTITY_DEF = 0x40;
    constexpr ptrdiff_t ENV_GET_TIME_DEF = 0x1B8;
    constexpr ptrdiff_t ENT_SET_TIME_DEF = 0x130;
}

struct EffectSlot {
    int id;
    ptrdiff_t slotA;
    ptrdiff_t slotB;
};

inline constexpr EffectSlot kSlots[] = {
    {0, 0x18, 0x20}, {1, 0x28, 0x30}, {2, 0x48, 0x50}, {3, 0x58, 0x60},
    {4, 0x68, 0x70}, {5, 0x88, 0x90}, {6, 0x98, 0xA0}, {7, 0xA8, 0xB0}, {8, 0xB8, 0xC0},
};

inline constexpr int kEffectCount = 9;

typedef void(__fastcall* WeatherTick_fn)(long long self, float dt);
typedef void(__fastcall* ActivateEffect_fn)(long long self, int id, long long* slotA, long long* slotB, float v);
typedef void(__fastcall* SetIntensity_fn)(long long particleMgr, int handle, float v);
typedef __m128(__fastcall* GetWeatherIntensity_fn)(long long weatherState);
typedef __m128(__fastcall* GetDustIntensity_fn)(long long weatherState);
typedef void(__fastcall* PPLayerUpdate_fn)(long long layerMgr, float dt);
typedef long long(__fastcall* GetLayerMeta_fn)(void* layerEntry);
typedef void(__fastcall* AtmosFogBlend_fn)(long long ctx, long long outParams);
typedef void(__fastcall* WeatherFrameUpdate_fn)(long long* self, float dt);
typedef void(__fastcall* ProcessWindState_fn)(long long self);
typedef void(__fastcall* WindPack_fn)(long long* windNodePtr, float* packedOut);
typedef void(__fastcall* CloudPack_fn)(
    long long self,
    long long* cloudNodePtr,
    float* packedOut,
    unsigned long long* p4,
    unsigned long long* p5,
    char p6,
    float driftX,
    float driftZ);
typedef long long*(__fastcall* FogReceiverGetter_fn)(long long provider);
typedef void(__fastcall* FogReceiverSet_fn)(long long* receiver, float value);
typedef double(__fastcall* EnvGetTimeOfDay_fn)(void* envMgr);
typedef void(__fastcall* EntitySetTimeOfDay_fn)(long long entity, float value);
typedef void*(__fastcall* NativeToastCreateString_fn)(const char* text);
typedef void(__fastcall* NativeToastPush_fn)(void* manager, void** messageHandle, unsigned int flags);
typedef void(__fastcall* NativeToastReleaseString_fn)(void* messageHandle);

inline WeatherTick_fn g_pOriginalTick = nullptr;
inline ActivateEffect_fn g_pActivateEffect = nullptr;
inline SetIntensity_fn g_pSetIntensity = nullptr;
inline GetWeatherIntensity_fn g_pOrigGetRainIntensity = nullptr;
inline GetWeatherIntensity_fn g_pOrigGetSnowIntensity = nullptr;
inline GetDustIntensity_fn g_pOrigGetDustIntensity = nullptr;
inline PPLayerUpdate_fn g_pOrigPPLayerUpdate = nullptr;
inline GetLayerMeta_fn g_pGetLayerMeta = nullptr;
inline AtmosFogBlend_fn g_pOrigAtmosFogBlend = nullptr;
inline WeatherFrameUpdate_fn g_pOrigWeatherFrameUpdate = nullptr;
inline ProcessWindState_fn g_pOrigProcessWindState = nullptr;
inline WindPack_fn g_pOrigWindPack = nullptr;
inline CloudPack_fn g_pOrigCloudPack = nullptr;
inline uintptr_t* g_pEnvManager = nullptr;
inline int* g_pNullSentinel = nullptr;
inline void** g_pWeatherTickVtableSlot = nullptr;
inline FogReceiverSet_fn g_pOrigFogSet[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
inline uintptr_t g_addrFogSet[5] = { 0, 0, 0, 0, 0 };
inline uintptr_t g_addrWeatherFrameUpdateResolved = 0;
inline std::atomic<float> g_forcedFogSet[5];
inline std::atomic<float> g_windMul{ 1.0f };
inline std::atomic<bool> g_fogSetHooksAttempted{ false };
inline std::atomic<bool> g_fogSetHooksInstalled{ false };
inline std::atomic<bool> g_timeLayoutReady{ false };
inline ptrdiff_t g_tdLowerLimit = TD::LOWER_LIMIT_DEF;
inline ptrdiff_t g_tdUpperLimit = TD::UPPER_LIMIT_DEF;
inline ptrdiff_t g_tdCurrentA = TD::CURRENT_A_DEF;
inline ptrdiff_t g_tdCurrentB = TD::CURRENT_B_DEF;
inline ptrdiff_t g_tdEnvGetEntity = TD::ENV_GET_ENTITY_DEF;
inline ptrdiff_t g_tdEnvGetTime = TD::ENV_GET_TIME_DEF;
inline ptrdiff_t g_tdEntSetTime = TD::ENT_SET_TIME_DEF;
inline uintptr_t g_addrTimeLowerHandler = 0;
inline uintptr_t g_addrTimeUpperHandler = 0;
inline uintptr_t g_addrTimeDebugHandler = 0;
inline NativeToastCreateString_fn g_pNativeToastCreateString = nullptr;
inline NativeToastPush_fn g_pNativeToastPush = nullptr;
inline NativeToastReleaseString_fn g_pNativeToastReleaseString = nullptr;
inline void** g_pNativeToastRootGlobal = nullptr;
inline uint32_t g_nativeToastOuterOffset = 0;
inline ptrdiff_t g_nativeToastManagerOffset = 0;

enum class RuntimeHealthState : uint8_t {
    Disabled = 0,
    Degraded = 1,
    Ready = 2,
};

enum class AobTargetId : uint8_t {
    WeatherTick = 0,
    GetRainIntensity,
    GetSnowIntensity,
    GetDustIntensity,
    ProcessWindState,
    ActivateEffect,
    SetIntensity,
    CloudPack,
    WindPack,
    PostProcessLayerUpdate,
    GetLayerMeta,
    WeatherFrameUpdate,
    AtmosFogBlend,
    EnvManagerPtr,
    NullSentinel,
    TimeStores,
    TimeDebugHandler,
    NativeToast,
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

inline std::array<RuntimeHealthEntry, static_cast<size_t>(AobTargetId::Count)> g_aobTargetHealth{};
inline std::array<RuntimeHealthEntry, static_cast<size_t>(RuntimeHealthGroup::Count)> g_runtimeGroupHealth{};
inline std::array<RuntimeHealthEntry, static_cast<size_t>(RuntimeFeatureId::Count)> g_runtimeFeatureHealth{};

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
bool RuntimeFeatureDegraded(RuntimeFeatureId id);
const char* RuntimeFeatureNote(RuntimeFeatureId id);

bool InstallHook(void* target, void* detour, void** trampoline, const char* name, bool required = true);
uintptr_t FindFunctionStartViaUnwind(uintptr_t pc);

struct SliderOverride {
    std::atomic<bool> active{ false };
    std::atomic<float> value{ 0.0f };

    void set(float v) {
        value.store(v);
        active.store(true);
    }

    void clear() {
        active.store(false);
        value.store(0.0f);
    }

    float get(float fallback) const {
        return active.load() ? value.load() : fallback;
    }
};

inline SliderOverride g_oRain;
inline SliderOverride g_oSnow;
inline SliderOverride g_oDust;
inline SliderOverride g_oFog;
inline SliderOverride g_oCloudSpdX;
inline SliderOverride g_oCloudSpdY;
inline SliderOverride g_oHighClouds;
inline SliderOverride g_oAtmoAlpha;
inline SliderOverride g_oExpCloud2C;
inline SliderOverride g_oExpCloud2D;
inline SliderOverride g_oExpNightSkyRot;
inline SliderOverride g_oCloudThk;
inline SliderOverride g_oNativeFog;
inline SliderOverride g_oWind;
inline SliderOverride g_oWindActual;
inline SliderOverride g_oSunDirX;
inline SliderOverride g_oSunDirY;
inline SliderOverride g_oMoonDirX;
inline SliderOverride g_oMoonDirY;
inline constexpr float kForceCloudsDefaultAmount = 0.25f;
inline std::atomic<bool> g_forceCloudsEnabled{ false };
inline std::atomic<float> g_forceCloudsAmount{ kForceCloudsDefaultAmount };
inline std::atomic<bool> g_forceClear{ false };
inline std::atomic<bool> g_noWind{ false };
inline std::atomic<bool> g_modEnabled{ true };
inline std::atomic<bool> g_modSuspendRequested{ false };
inline std::atomic<bool> g_timeCtrlActive{ false };
inline std::atomic<bool> g_timeFreeze{ false };
inline std::atomic<bool> g_timeApplyRequest{ false };
inline std::atomic<float> g_timeTargetHour{ 12.0f };
inline std::atomic<float> g_timeCurrentHour{ 12.0f };
inline std::atomic<float> g_timeOriginalHour{ 12.0f };
inline std::atomic<bool> g_timeOriginalHourValid{ false };
inline std::atomic<bool> g_timeDomainKnown{ false };
inline std::atomic<bool> g_timeDomainHours{ false };
inline std::atomic<bool> g_timeLimitsCaptured{ false };
inline std::atomic<float> g_timeBaseLower{ 0.0f };
inline std::atomic<float> g_timeBaseUpper{ 1.0f };
inline std::atomic<bool> g_timeFreezeApplied{ false };
inline std::atomic<int> g_timeSetHoldTicks{ 0 };
inline std::atomic<float> g_timeFrozenRaw{ -9999.0f };
inline std::atomic<bool> g_cloudBaseValid{ false };
inline std::atomic<float> g_cloudBaseTop{ 1.0f };
inline std::atomic<float> g_cloudBaseThick{ 1.0f };
inline std::atomic<float> g_cloudBaseBase{ 1.0f };
inline std::atomic<float> g_cloudBaseShapeA{ 1.0f };
inline std::atomic<float> g_cloudBaseShapeB{ 1.0f };
inline std::atomic<float> g_cloudBaseShapeC{ 1.0f };
inline std::atomic<bool> g_windPackBaseValid{ false };
inline std::atomic<float> g_windPackBase23{ 0.0f };
inline std::atomic<float> g_windPackBase24{ 0.0f };
inline std::atomic<float> g_windPackBase2F{ 0.0f };
inline std::atomic<float> g_windPackBase30{ 0.0f };
inline std::atomic<bool> g_windPackBase2CValid{ false };
inline std::atomic<float> g_windPackBase2C{ 0.0f };
inline std::atomic<bool> g_windPackBase2DValid{ false };
inline std::atomic<float> g_windPackBase2D{ 0.0f };
inline std::atomic<bool> g_windPackBase0AValid{ false };
inline std::atomic<float> g_windPackBase0A{ 0.0f };
inline std::atomic<bool> g_windPackBase11Valid{ false };
inline std::atomic<float> g_windPackBase11{ 0.0f };
inline std::atomic<bool> g_windPackBase17Valid{ false };
inline std::atomic<float> g_windPackBase17{ 0.0f };
inline std::atomic<bool> g_windNodeBaseValid{ false };
inline std::atomic<float> g_windNodeBaseSpeed{ 0.0f };
inline std::atomic<float> g_windNodeBaseGust{ 0.0f };
inline std::atomic<bool> g_atmoCelestialBaseValid{ false };
inline std::atomic<float> g_atmoBaseSunDirX{ 0.0f };
inline std::atomic<float> g_atmoBaseSunDirY{ 0.0f };
inline std::atomic<float> g_atmoBaseMoonDirX{ 0.0f };
inline std::atomic<float> g_atmoBaseMoonDirY{ 0.0f };
inline int g_activeWeather = -1;
inline std::atomic<bool> g_resetStopRequested{ false };
inline constexpr int kCustomWeather = 99;
inline char g_statusText[128] = "Ready";

struct ResolvedEnv {
    long long entity = 0;
    long long weatherState = 0;
    long long cloudNode = 0;
    long long windNode = 0;
    long long atmosphereNode = 0;
    long long particleMgr = 0;
    bool valid = false;
};

ResolvedEnv ResolveEnv();

template <typename T>
static T& At(long long base, ptrdiff_t off) {
    return *reinterpret_cast<T*>(base + off);
}

float Clamp01(float v);
float CloudXToSafeMul(float ui);
float NormalizeHour24(float h);
bool ResolveTimeContext(void*& outEnvMgr, long long& outEntity);
bool TryReadCurrentTimeRaw(void* envMgr, float& outRaw);
bool TrySetTimeRaw(long long entity, float raw);
void CaptureTimeLimitBaseline(long long entity);
void RestoreTimeLimitBaseline(long long entity);
float UIHourToEngineRaw(float hour);
bool TryReadCurrentHourFromEntity(long long entity, float& outHour);
void TickTimeControl();
void SuspendTimeControl();
void SetModEnabled(bool enabled);
void ToggleModEnabled();
void* ResolveNativeToastManager();
bool NativeToastReady();
void ShowNativeToast(const char* msg);
bool StartHotkeyService();
void StopHotkeyService();

bool RunAOBScan();
void RestoreRuntimePatches();
__m128 __fastcall Hooked_GetRainIntensity(long long ws);
__m128 __fastcall Hooked_GetSnowIntensity(long long ws);
__m128 __fastcall Hooked_GetDustIntensity(long long ws);
void __fastcall Hooked_PPLayerUpdate(long long layerMgr, float dt);
void __fastcall Hooked_AtmosFogBlend(long long ctx, long long outParams);
void __fastcall Hooked_WeatherFrameUpdate(long long* self, float dt);
void __fastcall Hooked_ProcessWindState(long long self);
void __fastcall Hooked_WindPack(long long* windNodePtr, float* packedOut);
void __fastcall Hooked_CloudPack(
    long long self,
    long long* cloudNodePtr,
    float* packedOut,
    unsigned long long* p4,
    unsigned long long* p5,
    char p6,
    float driftX,
    float driftZ);
void __fastcall Hooked_WeatherTick(long long self, float dt);
