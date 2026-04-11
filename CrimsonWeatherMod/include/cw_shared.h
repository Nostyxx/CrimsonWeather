#pragma once
// Crimson Weather shared runtime state

#include "pch.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <atomic>
#include <array>
#include <algorithm>
#include <string>
#include <Windows.h>
#include <Xinput.h>
#include <intrin.h>
#include <commctrl.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "xinput9_1_0.lib")
using std::min; using std::max;

#define MOD_NAME    "Crimson Weather"
#define MOD_VERSION "0.2.3"

// Logger
inline FILE* g_logFile    = nullptr;
inline bool  g_logEnabled = true;
void Log(const char* fmt, ...);

// INI config
struct Config {
    bool logEnabled = true;
    int hotkeyVK = VK_F9;
    int effectToggleVK = VK_F10;
    WORD controllerGuiToggleMask = WORD(XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_B);
    WORD controllerEffectToggleMask = WORD(XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_X);
    float uiScale = 1.0f;
    bool showGuiOnStartup = true;
};
inline Config g_cfg;
inline char g_pluginDir[MAX_PATH] = {};
inline constexpr float kUiScaleMin = 0.75f;
inline constexpr float kUiScaleMax = 3.00f;
float ClampUiScale(float v);
void BuildIniPath(char* outPath, size_t outSize);
int KeyNameToVK(const char* n);
std::string VKToKeyName(int vk);
WORD ControllerTokenToMask(const char* token);
WORD ParseControllerCombo(const char* text, WORD fallback);
bool IsControllerComboPressed(WORD buttons, WORD comboMask);
void ControllerComboToDisplayString(WORD mask, char* out, size_t outSize);
void LoadConfig(const char* dir);
void SaveConfigUIScale();
void SaveConfigShowGuiOnStartup();
void OpenLogFile(const char* dir);

namespace WCO {
    constexpr ptrdiff_t WIND_EVENT_A    = 0xD0;
    constexpr ptrdiff_t WIND_EVENT_B    = 0xD4;
    constexpr ptrdiff_t LERP_ALPHA     = 0xEC;
    constexpr ptrdiff_t BLEND_DIR_MULT = 0xF0;
    constexpr ptrdiff_t HANDLE_ARRAY   = 0xF4;  // int32[9]
    constexpr ptrdiff_t SOUND_RAIN     = 0xD8;
    constexpr ptrdiff_t SOUND_WIND     = 0xDC;  // active wind sound handle
    constexpr ptrdiff_t SOUND_SKYWIND  = 0xE0;  // active sky-wind sound handle
}
namespace CN {
    constexpr ptrdiff_t DUST_BASE      = 0x120; // used by GetDustIntensity fast path
    constexpr ptrdiff_t DUST_ADD       = 0x18C; // additive term in GetDustIntensity
    constexpr ptrdiff_t DUST_WIND_SCALE= 0x140; // scaled term in GetDustIntensity
    constexpr ptrdiff_t FOG_A       = 0x11C;
    constexpr ptrdiff_t CLOUD_TOP   = 0x124;
    constexpr ptrdiff_t CLOUD_THICK = 0x128;
    constexpr ptrdiff_t CLOUD_BASE  = 0x12C;
    constexpr ptrdiff_t CLOUD_SHAPE_A = 0x130;
    constexpr ptrdiff_t CLOUD_SHAPE_B = 0x134;
    constexpr ptrdiff_t CLOUD_SHAPE_C = 0x138;
    constexpr ptrdiff_t DUST_THRESH = 0x180;
    constexpr ptrdiff_t STORM_THRESH= 0x184;
    constexpr ptrdiff_t FOG_B       = 0x188;
}
namespace WN {
    constexpr ptrdiff_t DIR_X       = 0x18;
    constexpr ptrdiff_t DIR_Z       = 0x1C;
    constexpr ptrdiff_t TURB_DENS   = 0x60;
    constexpr ptrdiff_t TURB_SCALE  = 0x64;
    constexpr ptrdiff_t TURB_LIFT   = 0x68;
    constexpr ptrdiff_t SPEED       = 0x88;
    constexpr ptrdiff_t GUST        = 0x9C;
    constexpr ptrdiff_t SNOW_GROUND = 0xA8;
    constexpr ptrdiff_t CLOTH_WIND_X= 0xB8;
    constexpr ptrdiff_t CLOTH_WIND_Z= 0xBC;
    constexpr ptrdiff_t CLOUD_SCROLL_X = 0xD0;
    constexpr ptrdiff_t CLOUD_SCROLL_Z = 0xD4;
}
namespace TD {
    // Fallback constants
    constexpr ptrdiff_t LOWER_LIMIT_DEF    = 0x3CC;
    constexpr ptrdiff_t UPPER_LIMIT_DEF    = 0x3D0;
    constexpr ptrdiff_t CURRENT_A_DEF      = 0x3C8;
    constexpr ptrdiff_t CURRENT_B_DEF      = 0x3C4;
    constexpr ptrdiff_t ENV_GET_ENTITY_DEF = 0x40;
    constexpr ptrdiff_t ENV_GET_TIME_DEF   = 0x1B8;
    constexpr ptrdiff_t ENT_SET_TIME_DEF   = 0x130;
}

// Effect slots
struct EffectSlot { int id; ptrdiff_t slotA, slotB; };
inline constexpr EffectSlot kSlots[] = {
    {0,0x18,0x20},{1,0x28,0x30},{2,0x48,0x50},{3,0x58,0x60},
    {4,0x68,0x70},{5,0x88,0x90},{6,0x98,0xA0},{7,0xA8,0xB0},{8,0xB8,0xC0},
};
inline constexpr int kEffectCount = 9;

// Function typedefs
typedef void  (__fastcall* WeatherTick_fn)        (long long self, float dt);
typedef void  (__fastcall* ActivateEffect_fn)     (long long self, int id,
                                                    long long* slotA, long long* slotB, float v);
typedef void  (__fastcall* SetIntensity_fn)       (long long particleMgr, int handle, float v);
typedef float (__fastcall* GetWeatherIntensity_fn)(long long weatherState);
typedef float (__fastcall* GetDustIntensity_fn)   (long long weatherState, unsigned int p2);
typedef void  (__fastcall* PPLayerUpdate_fn)      (long long layerMgr, float dt);
typedef long long (__fastcall* GetLayerMeta_fn)   (void* layerEntry);
typedef void  (__fastcall* AtmosFogBlend_fn)      (long long ctx, long long outParams);
typedef void  (__fastcall* WeatherFrameUpdate_fn)  (long long* self, float dt);
typedef unsigned long long (__fastcall* ProcessWindState_fn)(long long self);
typedef float (__fastcall* WindPack_fn)(long long* windNodePtr, float* packedOut);
typedef void (__fastcall* CloudPack_fn)(long long self, long long* cloudNodePtr, float* packedOut,
                                        unsigned long long* p4, unsigned long long* p5, char p6,
                                        float driftX, float driftZ);
typedef long long* (__fastcall* FogReceiverGetter_fn)(long long provider);
typedef void (__fastcall* FogReceiverSet_fn)(long long* receiver, float value);
typedef HRESULT (WINAPI* CreateDXGIFactory_fn)(REFIID riid, void** ppFactory);
typedef HRESULT (WINAPI* CreateDXGIFactory2_fn)(UINT flags, REFIID riid, void** ppFactory);
typedef HRESULT (STDMETHODCALLTYPE* FactoryCreateSwapChain_fn)(
    IDXGIFactory* self, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain);
typedef HRESULT (STDMETHODCALLTYPE* Factory2CreateSwapChainForHwnd_fn)(
    IDXGIFactory2* self, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain);
typedef HRESULT (STDMETHODCALLTYPE* SwapChainPresent_fn)(IDXGISwapChain3* self, UINT syncInterval, UINT flags);
typedef HRESULT (STDMETHODCALLTYPE* SwapChainResizeBuffers_fn)(
    IDXGISwapChain3* self, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags);
typedef BOOL (WINAPI* SetCursorPos_fn)(int X, int Y);
typedef BOOL (WINAPI* ClipCursor_fn)(const RECT* lpRect);
typedef UINT (WINAPI* GetRawInputData_fn)(HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader);
typedef DWORD (WINAPI* XInputGetState_fn)(DWORD dwUserIndex, XINPUT_STATE* pState);
typedef double (__fastcall* EnvGetTimeOfDay_fn)(void* envMgr);
typedef void (__fastcall* EntitySetTimeOfDay_fn)(long long entity, float value);
typedef void* (__fastcall* NativeToastCreateString_fn)(const char* text);
typedef void (__fastcall* NativeToastPush_fn)(void* manager, void** messageHandle, unsigned int flags);
typedef void (__fastcall* NativeToastReleaseString_fn)(void* messageHandle);

// Resolved function pointers
inline WeatherTick_fn         g_pOriginalTick         = nullptr;
inline ActivateEffect_fn      g_pActivateEffect       = nullptr;
inline SetIntensity_fn        g_pSetIntensity         = nullptr;
inline GetWeatherIntensity_fn g_pOrigGetRainIntensity = nullptr;
inline GetWeatherIntensity_fn g_pOrigGetSnowIntensity = nullptr;
inline GetDustIntensity_fn    g_pOrigGetDustIntensity = nullptr;
inline PPLayerUpdate_fn       g_pOrigPPLayerUpdate    = nullptr;
inline GetLayerMeta_fn        g_pGetLayerMeta         = nullptr;
inline AtmosFogBlend_fn       g_pOrigAtmosFogBlend    = nullptr;
inline WeatherFrameUpdate_fn  g_pOrigWeatherFrameUpdate = nullptr;
inline ProcessWindState_fn    g_pOrigProcessWindState = nullptr;
inline WindPack_fn            g_pOrigWindPack         = nullptr;
inline CloudPack_fn           g_pOrigCloudPack        = nullptr;
inline uintptr_t*             g_pEnvManager           = nullptr;
inline int*                   g_pNullSentinel         = nullptr;
inline FogReceiverSet_fn      g_pOrigFogSet[5]        = { nullptr, nullptr, nullptr, nullptr, nullptr };
inline uintptr_t              g_addrFogSet[5]         = { 0, 0, 0, 0, 0 };
inline uintptr_t              g_addrWeatherFrameUpdateResolved = 0;
inline std::atomic<float>     g_forcedFogSet[5];
inline std::atomic<unsigned long long> g_fogSetCount[5];
inline std::atomic<float>     g_windMul{ 1.0f }; 
inline std::atomic<float>     g_fogSetLastIn[5];
inline std::atomic<float>     g_fogSetLastOut[5];
inline std::atomic<float>     g_fogPipeLastOut[5];
inline std::atomic<bool>      g_fogSetHooksAttempted  { false };
inline std::atomic<bool>      g_fogSetHooksInstalled  { false };
inline std::atomic<bool>      g_timeLayoutReady       { false };
inline ptrdiff_t              g_tdLowerLimit          = TD::LOWER_LIMIT_DEF;
inline ptrdiff_t              g_tdUpperLimit          = TD::UPPER_LIMIT_DEF;
inline ptrdiff_t              g_tdCurrentA            = TD::CURRENT_A_DEF;
inline ptrdiff_t              g_tdCurrentB            = TD::CURRENT_B_DEF;
inline ptrdiff_t              g_tdEnvGetEntity        = TD::ENV_GET_ENTITY_DEF;
inline ptrdiff_t              g_tdEnvGetTime          = TD::ENV_GET_TIME_DEF;
inline ptrdiff_t              g_tdEntSetTime          = TD::ENT_SET_TIME_DEF;
inline uintptr_t              g_addrTimeLowerHandler  = 0;
inline uintptr_t              g_addrTimeUpperHandler  = 0;
inline uintptr_t              g_addrTimeDebugHandler  = 0;
inline CreateDXGIFactory_fn   g_pOrigCreateDXGIFactory  = nullptr;
inline CreateDXGIFactory_fn   g_pOrigCreateDXGIFactory1 = nullptr;
inline CreateDXGIFactory2_fn  g_pOrigCreateDXGIFactory2 = nullptr;
inline FactoryCreateSwapChain_fn g_pOrigFactoryCreateSwapChain = nullptr;
inline Factory2CreateSwapChainForHwnd_fn g_pOrigFactory2CreateSwapChainForHwnd = nullptr;
inline SwapChainPresent_fn    g_pOrigSwapChainPresent   = nullptr;
inline SwapChainResizeBuffers_fn g_pOrigSwapChainResizeBuffers = nullptr;
inline SetCursorPos_fn        g_pOrigSetCursorPos       = nullptr;
inline ClipCursor_fn          g_pOrigClipCursor         = nullptr;
inline GetRawInputData_fn     g_pOrigGetRawInputData    = nullptr;
inline XInputGetState_fn      g_pOrigXInputGetState     = nullptr;
inline NativeToastCreateString_fn  g_pNativeToastCreateString = nullptr;
inline NativeToastPush_fn          g_pNativeToastPush = nullptr;
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

bool InstallHook(void*t,void*d,void**tr,const char*n,bool req=true);
uintptr_t FindFunctionStartViaUnwind(uintptr_t pc);

struct SliderOverride {
    std::atomic<bool>  active { false };
    std::atomic<float> value  { 0.0f };
    void set(float v)  { value.store(v); active.store(true); }
    void clear()       { active.store(false); value.store(0.0f); }
    float get(float presetVal) const {
        return active.load() ? value.load() : presetVal;
    }
};

inline SliderOverride g_oRain;       // direct rain particle intensity (0-1)
inline SliderOverride g_oSnow;       // direct snow particle intensity (0-1)
inline SliderOverride g_oDust;       // direct dust/wind intensity (0-2)
inline SliderOverride g_oFog;        // fog total (0-100 overdrive)
inline SliderOverride g_oCloudSpdX;  // cloud height / shape separation control
inline SliderOverride g_oCloudSpdY;  // cloud density / fill control
inline SliderOverride g_oHighClouds; // mid-cloud layer multiplier (1.0 = native)
inline SliderOverride g_oAtmoAlpha;        // packed cloud field 0x2F
inline SliderOverride g_oExpCloud2C; // experiment packed cloud field 0x2C
inline SliderOverride g_oExpCloud2D; // experiment packed cloud field 0x2D
inline SliderOverride g_oExpNightSkyRot; // experiment packed atmosphere field 0x0A
inline SliderOverride g_oCloudThk;   // puddle scale (0-1), inverse puddle size
inline SliderOverride g_oWind;       // wind speed m/s (0-50)
inline constexpr float kForceCloudsDefaultAmount = 0.25f;
inline std::atomic<bool>  g_forceCloudsEnabled{ false };
inline std::atomic<float> g_forceCloudsAmount{ kForceCloudsDefaultAmount };
inline std::atomic<bool> g_forceClear   { false };  // force clear sky channels
inline std::atomic<bool> g_noWind       { false };  // hard disable wind when enabled
inline std::atomic<bool> g_modEnabled   { true };
inline std::atomic<bool> g_modSuspendRequested { false };
// Visual Time Override currently relies on both flags:
inline std::atomic<bool>  g_timeCtrlActive     { false }; // time slider authority
inline std::atomic<bool>  g_timeFreeze          { false }; // stop time progression
inline std::atomic<bool>  g_timeApplyRequest    { false }; // one-shot apply in non-freeze mode
inline std::atomic<float> g_timeTargetHour      { 12.0f }; // UI domain: 0..24
inline std::atomic<float> g_timeCurrentHour     { 12.0f }; // sampled from runtime
inline std::atomic<bool>  g_timeDomainKnown     { false }; // false until first valid sample
inline std::atomic<bool>  g_timeDomainHours     { false }; // true: raw is hours, false: raw is 0..1
inline std::atomic<bool>  g_timeLimitsCaptured  { false };
inline std::atomic<float> g_timeBaseLower       { 0.0f };
inline std::atomic<float> g_timeBaseUpper       { 1.0f };
inline std::atomic<bool>  g_timeFreezeApplied   { false }; 
inline std::atomic<int>   g_timeSetHoldTicks    { 0 };    
inline std::atomic<float> g_timeFrozenRaw       { -9999.0f };
inline std::atomic<bool>  g_cloudBaseValid      { false };
inline std::atomic<float> g_cloudBaseTop        { 1.0f };
inline std::atomic<float> g_cloudBaseThick      { 1.0f };
inline std::atomic<float> g_cloudBaseBase       { 1.0f };
inline std::atomic<float> g_cloudBaseShapeA     { 1.0f };
inline std::atomic<float> g_cloudBaseShapeB     { 1.0f };
inline std::atomic<float> g_cloudBaseShapeC     { 1.0f };
inline std::atomic<bool>  g_windPackBaseValid   { false };
inline std::atomic<float> g_windPackBase23      { 0.0f };
inline std::atomic<float> g_windPackBase24      { 0.0f };
inline std::atomic<float> g_windPackBase2F      { 0.0f };
inline std::atomic<float> g_windPackBase30      { 0.0f };
inline std::atomic<bool>  g_windPackBase2CValid { false };
inline std::atomic<float> g_windPackBase2C      { 0.0f };
inline std::atomic<bool>  g_windPackBase2DValid { false };
inline std::atomic<float> g_windPackBase2D      { 0.0f };
inline std::atomic<bool>  g_windPackBase0AValid { false };
inline std::atomic<float> g_windPackBase0A      { 0.0f };

// Global state
inline int g_activeWeather = -1;  
inline std::atomic<bool> g_resetStopRequested{ false };

inline constexpr int kCustomWeather = 99;

bool AnySliderActive();
bool AnyCustomWeatherSliderActive();

// UI state
inline char g_statusText[128] = "Ready";
inline bool g_menuOpen = true;
inline bool g_dxgiHooksInstalled = false;
inline bool g_factoryHooksInstalled = false;
inline bool g_swapChainHooksInstalled = false;
inline int  g_uiTabIndex = 0;          // 0=Weather 1=Cloud 2=Atmosphere 3=Detail 4=Experiment 5=Settings
inline bool g_uiTabSelectRequested = false;
inline WORD g_padButtonsPrev = 0;
inline WORD g_dualSenseButtonsPrev = 0;
inline WORD g_dualSenseButtons = 0;
inline WORD g_padUiPollPrevButtons = 0;
enum class OverlayPadSource {
    None,
    XInput,
    GameInput,
};
inline OverlayPadSource g_padUiPollSource = OverlayPadSource::None;
inline ULONGLONG g_dualSenseLastInputMs = 0;
inline int  g_uiFocusByTab[6] = { 0, 0, 0, 0, 0, 0 };
inline std::atomic<ULONGLONG> g_uiInputSuppressUntil{ 0 };
inline std::atomic<ULONGLONG> g_uiConsumeUntil{ 0 };
inline std::atomic<bool> g_uiControllerMode{ false };
inline std::atomic<bool> g_uiPresetPopupOpen{ false };  
inline std::atomic<bool> g_uiRequestClosePopup{ false }; 
inline ULONGLONG g_padHoldNextMs[4] = { 0, 0, 0, 0 }; 
inline ULONGLONG g_padHoldStartMs[4] = { 0, 0, 0, 0 }; 
inline ImGuiStyle g_imguiBaseStyle = {};
inline bool g_imguiBaseStyleValid = false;
inline float g_uiScaleApplied = 1.0f;
inline bool g_uiScaleConfigDirty = false;
inline ULONGLONG g_uiScaleLastChangeMs = 0;
inline bool g_uiScaleWindowResizePending = false;
struct PadFrameCmd {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool a = false;
    float leftScale = 1.0f;
    float rightScale = 1.0f;
};
inline PadFrameCmd g_padCmd;

inline HWND   g_gameHwnd = nullptr;
inline WNDPROC g_origGameWndProc = nullptr;
inline IDXGISwapChain3* g_swapChain3 = nullptr;
inline ID3D12Device* g_dx12Device = nullptr;
inline ID3D12CommandQueue* g_dx12Queue = nullptr;
inline ID3D12DescriptorHeap* g_dx12RtvHeap = nullptr;
inline ID3D12DescriptorHeap* g_dx12SrvHeap = nullptr;
inline ID3D12GraphicsCommandList* g_dx12CmdList = nullptr;
inline ID3D12Fence* g_dx12Fence = nullptr;
inline HANDLE g_dx12FenceEvent = nullptr;
inline UINT64 g_dx12FenceLastValue = 0;
inline bool g_imguiReady = false;
inline bool g_imguiInitFailed = false;
inline DXGI_FORMAT g_swapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

inline constexpr UINT kMaxFramesInFlight = 8;
inline UINT g_frameCount = 0;
inline UINT g_rtvDescriptorSize = 0;
inline UINT g_srvDescriptorSize = 0;
inline UINT g_srvNextFree = 0;
struct Dx12FrameCtx {
    ID3D12CommandAllocator* cmdAllocator = nullptr;
    ID3D12Resource* renderTarget = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
    UINT64 fenceValue = 0;
};
inline Dx12FrameCtx g_dx12Frames[kMaxFramesInFlight];

void GUI_SetStatus(const char* msg);
void ApplyUiScale(float scale);
void SyncMenuCursorState();
void ResetAllSliders();

// Env pointer chain
struct ResolvedEnv {
    long long entity, weatherState, cloudNode, windNode, particleMgr;
    bool valid;
};
ResolvedEnv ResolveEnv();
template<typename T> static T& At(long long base, ptrdiff_t off) {
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

void SetupInputHooks();
bool RunAOBScan();
void UpdateControllerNavInput();
float __fastcall Hooked_GetRainIntensity(long long ws);
float __fastcall Hooked_GetSnowIntensity(long long ws);
float __fastcall Hooked_GetDustIntensity(long long ws, unsigned int p2);
void __fastcall Hooked_PPLayerUpdate(long long layerMgr, float dt);
void __fastcall Hooked_AtmosFogBlend(long long ctx, long long outParams);
void __fastcall Hooked_WeatherFrameUpdate(long long* self, float dt);
unsigned long long __fastcall Hooked_ProcessWindState(long long self);
float __fastcall Hooked_WindPack(long long* windNodePtr, float* packedOut);
void __fastcall Hooked_CloudPack(long long self, long long* cloudNodePtr, float* packedOut,
                                 unsigned long long* p4, unsigned long long* p5, char p6,
                                 float driftX, float driftZ);
void __fastcall Hooked_WeatherTick(long long self, float dt);
