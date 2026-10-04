#pragma once

#include <Windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>

struct Config {
    bool logEnabled = true;
    bool autoStart = true;
    bool autoSaved = false;
    bool toastNotification = true;
    bool communityEnabled = true;
    bool updaterEnabled = true;
    bool updaterAutoDownload = false;
    bool textureSwitcherEnabled = true;
    int textureSwitcherAnimatedTextureGpuSlots = 12;
    int textureSwitcherAnimatedMoonGpuSlots = 12;
    bool realGameTimeEnabled = false;
    float realGameTimeDayScale = 1.0f;
    float realGameTimeNightScale = 1.0f;
    int effectToggleVK = VK_F10;
    WORD controllerEffectToggleMask = 0;
    bool reshadeDiagnostics = false;
};

#if defined(CW_DEV_BUILD)
enum class DevLaunchOption : uint8_t {
    Full = 0,
    None,
    TextureHook,
    WeatherTickHook,
    IntensityHooks,
    WindHooks,
    FrameHooks,
    RegionHook,
};

const char* DevLaunchOptionName(DevLaunchOption option);
const char* DevLaunchOptionDescription(DevLaunchOption option);
DevLaunchOption ParseDevLaunchOption(const char* text);
bool DevLaunchOptionUsesTextureHook(DevLaunchOption option);
bool DevLaunchOptionUsesRuntimeStartup(DevLaunchOption option);
bool DevLaunchOptionBypassesStartupHealth(DevLaunchOption option);
#endif

extern Config g_cfg;
#if defined(CW_DEV_BUILD)
extern std::atomic<DevLaunchOption> g_devLaunchOption;
#endif
extern char g_pluginDir[MAX_PATH];

void BuildIniPath(char* outPath, size_t outSize);
int KeyNameToVK(const char* name);
WORD ControllerTokenToMask(const char* token);
WORD ParseControllerCombo(const char* text, WORD fallback);
bool IsControllerComboPressed(WORD buttons, WORD comboMask);
void LoadConfig(const char* dir);
void SaveGeneralConfig();
void SaveWindOnlyConfig();
