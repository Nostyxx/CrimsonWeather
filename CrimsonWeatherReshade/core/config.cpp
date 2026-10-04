#include "pch.h"

#include "runtime_shared.h"

#include <cctype>
#include <fstream>
#include <vector>

Config g_cfg{};
#if defined(CW_DEV_BUILD)
std::atomic<DevLaunchOption> g_devLaunchOption{ DevLaunchOption::Full };
#endif
char g_pluginDir[MAX_PATH] = {};

namespace {

void RemoveIniSectionByName(const char* path, const char* sectionName) {
    if (!path || !path[0] || !sectionName || !sectionName[0]) {
        return;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return;
    }

    std::vector<std::string> lines;
    std::string line;
    bool skipping = false;
    bool removed = false;

    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (!line.empty() && line.front() == '[' && line.back() == ']') {
            const std::string header = line.substr(1, line.size() - 2);
            if (_stricmp(header.c_str(), sectionName) == 0) {
                skipping = true;
                removed = true;
                continue;
            }
            skipping = false;
        }

        if (!skipping) {
            lines.push_back(line);
        }
    }
    in.close();

    if (!removed) {
        return;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return;
    }

    for (size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) {
            out << "\r\n";
        }
    }
}

bool IniKeyExists(const char* path, const char* sectionName, const char* keyName) {
    if (!path || !path[0] || !sectionName || !sectionName[0] || !keyName || !keyName[0]) {
        return false;
    }

    char section[8192] = {};
    const DWORD len = GetPrivateProfileSectionA(sectionName, section, static_cast<DWORD>(sizeof(section)), path);
    if (len == 0) {
        return false;
    }

    const size_t keyLen = strlen(keyName);
    for (const char* entry = section; *entry; entry += strlen(entry) + 1) {
        if (_strnicmp(entry, keyName, keyLen) == 0 && entry[keyLen] == '=') {
            return true;
        }
    }
    return false;
}

void WriteDefaultConfig(const char* path) {
    if (!path || !path[0]) {
        return;
    }

    WritePrivateProfileStringA("General", "LogEnabled", "0", path);
    WritePrivateProfileStringA("General", "AutoStart", "1", path);
    WritePrivateProfileStringA("General", "AutoSaved", "0", path);
    WritePrivateProfileStringA("General", "ToastNotification", "1", path);
    WritePrivateProfileStringA("General", "ExtendedSliderRange", "0", path);
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
#if !defined(CW_WIND_ONLY)
    WritePrivateProfileStringA("Preset", "LastPreset", "", path);
    WritePrivateProfileStringA("TimeSchedule", "Enabled", "0", path);
    WritePrivateProfileStringA("TimeSchedule", "TimeSource", "VisualTimeOverride", path);
    WritePrivateProfileStringA("TimeSchedule", "EntryCount", "0", path);
    WritePrivateProfileStringA("Community", "Enabled", "1", path);
    WritePrivateProfileStringA("Updater", "Enabled", "1", path);
    WritePrivateProfileStringA("Updater", "AutoDownload", "0", path);
    WritePrivateProfileStringA("TextureSwitcher", "Enabled", "1", path);
    WritePrivateProfileStringA("TextureSwitcher", "AnimatedTextureGpuSlots", "12", path);
    WritePrivateProfileStringA("RealGameTime", "Enabled", "0", path);
    WritePrivateProfileStringA("RealGameTime", "DayScale", "1.0000", path);
    WritePrivateProfileStringA("RealGameTime", "NightScale", "1.0000", path);
#else
    WritePrivateProfileStringA("Wind", "Multiplier", "1.0000", path);
#endif
#if defined(CW_DEV_BUILD)
    WritePrivateProfileStringA("Dev", "LaunchOption", "full", path);
    WritePrivateProfileStringA(
        "Dev",
        "_LaunchOptionValues",
        "full, none, texturehook, weathertickhook, intensityhooks, windhooks, framehooks, foghooks, regionhook",
        path);
#endif
}

void PatchMissingConfigKeys(const char* path) {
    if (!path || !path[0]) {
        return;
    }

    char buf[64] = {};
    if (GetPrivateProfileStringA("General", "AutoStart", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("General", "AutoStart", "1", path);
    }
    if (GetPrivateProfileStringA("General", "AutoSaved", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("General", "AutoSaved", "0", path);
    }
    if (GetPrivateProfileStringA("General", "ToastNotification", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("General", "ToastNotification", "1", path);
    }
    if (!IniKeyExists(path, "General", "HotkeyToggleEffect")) {
        WritePrivateProfileStringA("General", "HotkeyToggleEffect", "F10", path);
    }
    if (GetPrivateProfileStringA("General", "ExtendedSliderRange", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("General", "ExtendedSliderRange", "0", path);
    }
    if (GetPrivateProfileStringA("General", "_HotkeyOptions", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA(
            "General",
            "_HotkeyOptions",
            "F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, or single letter A-Z",
            path);
    }
    if (!IniKeyExists(path, "Hotkeys", "ControllerToggleEffect")) {
        WritePrivateProfileStringA("Hotkeys", "ControllerToggleEffect", "dpad_down+a", path);
    }
    if (GetPrivateProfileStringA("Hotkeys", "_ControllerHotkeyOptions", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA(
            "Hotkeys",
            "_ControllerHotkeyOptions",
            "Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back",
            path);
    }
#if defined(CW_WIND_ONLY)
    WritePrivateProfileStringA("Preset", nullptr, nullptr, path);
    RemoveIniSectionByName(path, "Preset");
    if (GetPrivateProfileStringA("Wind", "Multiplier", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("Wind", "Multiplier", "1.0000", path);
    }
#else
    GetPrivateProfileStringA("Preset", "LastPreset", "", buf, sizeof(buf), path);
    WritePrivateProfileStringA("Preset", "LastPreset", buf, path);
    if (GetPrivateProfileStringA("TimeSchedule", "Enabled", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("TimeSchedule", "Enabled", "0", path);
    }
    if (GetPrivateProfileStringA("TimeSchedule", "TimeSource", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("TimeSchedule", "TimeSource", "VisualTimeOverride", path);
    }
    if (GetPrivateProfileStringA("TimeSchedule", "EntryCount", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("TimeSchedule", "EntryCount", "0", path);
    }
    if (GetPrivateProfileStringA("Community", "Enabled", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("Community", "Enabled", "1", path);
    }
    if (GetPrivateProfileStringA("Updater", "Enabled", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("Updater", "Enabled", "1", path);
    }
    if (GetPrivateProfileStringA("Updater", "AutoDownload", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("Updater", "AutoDownload", "0", path);
    }
    if (GetPrivateProfileStringA("TextureSwitcher", "Enabled", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("TextureSwitcher", "Enabled", "1", path);
    }
    if (GetPrivateProfileStringA("TextureSwitcher", "AnimatedTextureGpuSlots", "", buf, sizeof(buf), path) == 0) {
        char legacyGpuSlots[64] = {};
        GetPrivateProfileStringA("TextureSwitcher", "AnimatedMoonGpuSlots", "12", legacyGpuSlots, sizeof(legacyGpuSlots), path);
        WritePrivateProfileStringA("TextureSwitcher", "AnimatedTextureGpuSlots", legacyGpuSlots, path);
    }
    if (GetPrivateProfileStringA("RealGameTime", "Enabled", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("RealGameTime", "Enabled", "0", path);
    }
    if (GetPrivateProfileStringA("RealGameTime", "DayScale", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("RealGameTime", "DayScale", "1.0000", path);
    }
    if (GetPrivateProfileStringA("RealGameTime", "NightScale", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("RealGameTime", "NightScale", "1.0000", path);
    }
#endif
#if defined(CW_DEV_BUILD)
    if (GetPrivateProfileStringA("Dev", "LaunchOption", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA("Dev", "LaunchOption", "full", path);
    }
    if (GetPrivateProfileStringA("Dev", "_LaunchOptionValues", "", buf, sizeof(buf), path) == 0) {
        WritePrivateProfileStringA(
            "Dev",
            "_LaunchOptionValues",
            "full, none, texturehook, weathertickhook, intensityhooks, windhooks, framehooks, foghooks, regionhook",
            path);
    }
#endif

    WritePrivateProfileStringA("General", "HotkeyToggleGUI", nullptr, path);
    WritePrivateProfileStringA("Hotkeys", "ControllerHotkeyToggleGUI", nullptr, path);
    WritePrivateProfileStringA("UI", "Scale", nullptr, path);
    WritePrivateProfileStringA("UI", "ShowOnStartup", nullptr, path);
    WritePrivateProfileStringA("Diagnostics", "ReShadeVerbose", nullptr, path);
}

} // namespace

#if defined(CW_DEV_BUILD)
namespace {

void NormalizeDevLaunchOptionText(const char* text, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }

    out[0] = '\0';
    if (!text) {
        return;
    }

    size_t written = 0;
    for (const char* p = text; *p && written + 1 < outSize; ++p) {
        const unsigned char ch = static_cast<unsigned char>(*p);
        if (std::isalnum(ch)) {
            out[written++] = static_cast<char>(std::tolower(ch));
        }
    }
    out[written] = '\0';
}

} // namespace

const char* DevLaunchOptionName(DevLaunchOption option) {
    switch (option) {
    case DevLaunchOption::None:
        return "none";
    case DevLaunchOption::TextureHook:
        return "texturehook";
    case DevLaunchOption::WeatherTickHook:
        return "weathertickhook";
    case DevLaunchOption::IntensityHooks:
        return "intensityhooks";
    case DevLaunchOption::WindHooks:
        return "windhooks";
    case DevLaunchOption::FrameHooks:
        return "framehooks";
    case DevLaunchOption::RegionHook:
        return "regionhook";
    case DevLaunchOption::Full:
    default:
        return "full";
    }
}

const char* DevLaunchOptionDescription(DevLaunchOption option) {
    switch (option) {
    case DevLaunchOption::None:
        return "DEV isolation: no texture hook and no runtime hooks.";
    case DevLaunchOption::TextureHook:
        return "DEV isolation: D3D12 moon/milkyway texture hook only.";
    case DevLaunchOption::WeatherTickHook:
        return "DEV isolation: WeatherTick hook only.";
    case DevLaunchOption::IntensityHooks:
        return "DEV isolation: rain/snow/dust intensity hooks only.";
    case DevLaunchOption::WindHooks:
        return "DEV isolation: ProcessWindState and WindPack hooks only.";
    case DevLaunchOption::FrameHooks:
        return "DEV isolation: production SceneFrameUpdate hook only.";
    case DevLaunchOption::RegionHook:
        return "DEV isolation: minimap region hook only.";
    case DevLaunchOption::Full:
    default:
        return "Normal DEV build: all hooks enabled.";
    }
}

DevLaunchOption ParseDevLaunchOption(const char* text) {
    char normalized[64] = {};
    NormalizeDevLaunchOptionText(text, normalized, sizeof(normalized));

    if (normalized[0] == '\0' || strcmp(normalized, "full") == 0 || strcmp(normalized, "all") == 0) {
        return DevLaunchOption::Full;
    }
    if (strcmp(normalized, "none") == 0 || strcmp(normalized, "off") == 0 || strcmp(normalized, "disabled") == 0) {
        return DevLaunchOption::None;
    }
    if (strcmp(normalized, "texturehook") == 0 || strcmp(normalized, "texture") == 0 ||
        strcmp(normalized, "moonhook") == 0 || strcmp(normalized, "moontexturehook") == 0) {
        return DevLaunchOption::TextureHook;
    }
    if (strcmp(normalized, "weathertickhook") == 0 || strcmp(normalized, "weathertick") == 0) {
        return DevLaunchOption::WeatherTickHook;
    }
    if (strcmp(normalized, "intensityhooks") == 0 || strcmp(normalized, "intensityhook") == 0 ||
        strcmp(normalized, "intensity") == 0) {
        return DevLaunchOption::IntensityHooks;
    }
    if (strcmp(normalized, "windhooks") == 0 || strcmp(normalized, "windhook") == 0 ||
        strcmp(normalized, "wind") == 0) {
        return DevLaunchOption::WindHooks;
    }
    if (strcmp(normalized, "framehooks") == 0 || strcmp(normalized, "framehook") == 0 ||
        strcmp(normalized, "sceneframehook") == 0 || strcmp(normalized, "sceneframe") == 0 ||
        strcmp(normalized, "scenehooks") == 0 || strcmp(normalized, "scenehook") == 0 ||
        strcmp(normalized, "sceneupdatehook") == 0 || strcmp(normalized, "sceneupdate") == 0 ||
        strcmp(normalized, "frame") == 0 || strcmp(normalized, "scene") == 0) {
        return DevLaunchOption::FrameHooks;
    }
    if (strcmp(normalized, "regionhook") == 0 || strcmp(normalized, "regionhooks") == 0 ||
        strcmp(normalized, "minimaphook") == 0 || strcmp(normalized, "minimap") == 0 ||
        strcmp(normalized, "region") == 0) {
        return DevLaunchOption::RegionHook;
    }
    return DevLaunchOption::Full;
}

bool DevLaunchOptionUsesTextureHook(DevLaunchOption option) {
    return option == DevLaunchOption::Full || option == DevLaunchOption::TextureHook;
}

bool DevLaunchOptionUsesRuntimeStartup(DevLaunchOption option) {
    return option != DevLaunchOption::None && option != DevLaunchOption::TextureHook;
}

bool DevLaunchOptionBypassesStartupHealth(DevLaunchOption option) {
    return option != DevLaunchOption::Full;
}

#endif

void BuildIniPath(char* outPath, size_t outSize) {
    if (!outPath || outSize == 0) {
        return;
    }
    if (g_pluginDir[0]) {
        sprintf_s(outPath, outSize, "%s\\%s", g_pluginDir, MOD_CONFIG_FILE);
        return;
    }
    strcpy_s(outPath, outSize, MOD_CONFIG_FILE);
}

int KeyNameToVK(const char* name) {
    if (!name) {
        return VK_F10;
    }
    if (!name[0]) {
        return 0;
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
    if (!text) {
        return fallback;
    }
    if (!text[0]) {
        return 0;
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
    GetPrivateProfileStringA("General", "AutoStart", "1", buf, sizeof(buf), path);
    g_cfg.autoStart = atoi(buf) != 0;
    GetPrivateProfileStringA("General", "AutoSaved", "0", buf, sizeof(buf), path);
    g_cfg.autoSaved = atoi(buf) != 0;
    GetPrivateProfileStringA("General", "ToastNotification", "1", buf, sizeof(buf), path);
    g_cfg.toastNotification = atoi(buf) != 0;
    GetPrivateProfileStringA("General", "ExtendedSliderRange", "0", buf, sizeof(buf), path);
    g_extendedSliderRange.store(atoi(buf) != 0);
    GetPrivateProfileStringA("General", "HotkeyToggleEffect", "F10", buf, sizeof(buf), path);
    g_cfg.effectToggleVK = KeyNameToVK(buf);
    GetPrivateProfileStringA("Hotkeys", "ControllerToggleEffect", "dpad_down+a", buf, sizeof(buf), path);
    g_cfg.controllerEffectToggleMask = ParseControllerCombo(buf, static_cast<WORD>(0x0002 | 0x4000));
    g_cfg.reshadeDiagnostics = false;
#if !defined(CW_WIND_ONLY)
    GetPrivateProfileStringA("Community", "Enabled", "1", buf, sizeof(buf), path);
    g_cfg.communityEnabled = atoi(buf) != 0;
    GetPrivateProfileStringA("Updater", "Enabled", "1", buf, sizeof(buf), path);
    g_cfg.updaterEnabled = atoi(buf) != 0;
    GetPrivateProfileStringA("Updater", "AutoDownload", "0", buf, sizeof(buf), path);
    g_cfg.updaterAutoDownload = atoi(buf) != 0;
    GetPrivateProfileStringA("TextureSwitcher", "Enabled", "1", buf, sizeof(buf), path);
    g_cfg.textureSwitcherEnabled = atoi(buf) != 0;
    GetPrivateProfileStringA("TextureSwitcher", "AnimatedTextureGpuSlots", "", buf, sizeof(buf), path);
    if (buf[0] == '\0') {
        GetPrivateProfileStringA("TextureSwitcher", "AnimatedMoonGpuSlots", "12", buf, sizeof(buf), path);
    }
    g_cfg.textureSwitcherAnimatedTextureGpuSlots = min(120, max(4, atoi(buf)));
    g_cfg.textureSwitcherAnimatedMoonGpuSlots = g_cfg.textureSwitcherAnimatedTextureGpuSlots;
    GetPrivateProfileStringA("RealGameTime", "Enabled", "0", buf, sizeof(buf), path);
    g_cfg.realGameTimeEnabled = atoi(buf) != 0;
    GetPrivateProfileStringA("RealGameTime", "DayScale", "1.0000", buf, sizeof(buf), path);
    g_cfg.realGameTimeDayScale = min(60.0f, max(0.01f, static_cast<float>(atof(buf))));
    g_realGameTimeDayScale.store(g_cfg.realGameTimeDayScale);
    GetPrivateProfileStringA("RealGameTime", "NightScale", "1.0000", buf, sizeof(buf), path);
    g_cfg.realGameTimeNightScale = min(60.0f, max(0.01f, static_cast<float>(atof(buf))));
    g_realGameTimeNightScale.store(g_cfg.realGameTimeNightScale);
#endif
#if defined(CW_DEV_BUILD)
    char devBuf[96] = {};
    GetPrivateProfileStringA("Dev", "LaunchOption", "full", devBuf, sizeof(devBuf), path);
    const DevLaunchOption launchOption = ParseDevLaunchOption(devBuf);
    g_devLaunchOption.store(launchOption);
    Log("[dev] LaunchOption=%s (%s)\n",
        DevLaunchOptionName(launchOption),
        DevLaunchOptionDescription(launchOption));

#endif
#if defined(CW_WIND_ONLY)
    GetPrivateProfileStringA("Wind", "Multiplier", "1.0000", buf, sizeof(buf), path);
    g_windMul.store(min(15.0f, max(0.0f, static_cast<float>(atof(buf)))));
#endif
}

void SaveGeneralConfig() {
    char path[MAX_PATH] = {};
    BuildIniPath(path, sizeof(path));
    WritePrivateProfileStringA("General", "AutoSaved", g_cfg.autoSaved ? "1" : "0", path);
    WritePrivateProfileStringA("General", "ToastNotification", g_cfg.toastNotification ? "1" : "0", path);
    WritePrivateProfileStringA("General", "ExtendedSliderRange", g_extendedSliderRange.load() ? "1" : "0", path);
#if !defined(CW_WIND_ONLY)
    WritePrivateProfileStringA("Updater", "AutoDownload", g_cfg.updaterAutoDownload ? "1" : "0", path);
    WritePrivateProfileStringA("RealGameTime", "Enabled", g_cfg.realGameTimeEnabled ? "1" : "0", path);
    char timeScale[32] = {};
    sprintf_s(timeScale, "%.4f", min(60.0f, max(0.01f, g_realGameTimeDayScale.load())));
    WritePrivateProfileStringA("RealGameTime", "DayScale", timeScale, path);
    sprintf_s(timeScale, "%.4f", min(60.0f, max(0.01f, g_realGameTimeNightScale.load())));
    WritePrivateProfileStringA("RealGameTime", "NightScale", timeScale, path);
#endif
}

void SaveWindOnlyConfig() {
#if defined(CW_WIND_ONLY)
    char path[MAX_PATH] = {};
    BuildIniPath(path, sizeof(path));
    char value[32] = {};
    sprintf_s(value, "%.4f", min(15.0f, max(0.0f, g_windMul.load())));
    WritePrivateProfileStringA("Wind", "Multiplier", value, path);
#endif
}
