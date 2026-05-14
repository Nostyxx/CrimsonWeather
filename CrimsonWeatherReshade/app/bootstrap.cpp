#include "pch.h"

#include "sky_texture_override.h"
#include "overlay_bridge.h"
#include "preset_service.h"
#include "runtime_shared.h"

extern "C" BOOL ReserveBufferBlock(LPVOID pOrigin);

namespace {

std::atomic<bool> g_initialized{ false };
std::atomic<bool> g_minHookInitialized{ false };
std::atomic<bool> g_nextStartIsAuto{ false };

bool IsTargetProcess() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return wcsstr(path, L"CrimsonDesert.exe") != nullptr;
}

void ResolveModuleDirectory(HMODULE module, char* outDir, size_t outDirSize) {
    if (!outDir || outDirSize == 0) {
        return;
    }

    outDir[0] = '\0';
    GetModuleFileNameA(module, outDir, static_cast<DWORD>(outDirSize));
    char* slash = strrchr(outDir, '\\');
    if (slash) {
        *slash = '\0';
    }
}

void MarkStartupFailed(const char* status) {
    if (status && status[0]) {
        GUI_SetStatus(status);
    }
    StartupSetStep(StartupStepId::Failed, g_startupStepIndex.load(), status ? status : "Startup failed");
    g_addonStartupState.store(AddonStartupState::Failed);
}

void CleanupFailedStart() {
    RestoreRuntimePatches();
    StopHotkeyService();
    if (g_minHookInitialized.load()) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_RemoveHook(MH_ALL_HOOKS);
    }
}

void PrimeMinHookRelayBlock() {
    const MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        Log("[W] MinHook early init failed: %d\n", static_cast<int>(mhStatus));
        return;
    }

    g_minHookInitialized.store(true);
    void* gameBase = GetModuleHandle(nullptr);
    const BOOL reserved = ReserveBufferBlock(gameBase);
    Log(reserved
        ? "[i] MinHook relay block reserved near game base\n"
        : "[W] MinHook relay block reserve failed near game base\n");
}

DWORD WINAPI StartThread(void*) {
    AddonStartupState expected = AddonStartupState::NotStarted;
    if (!g_addonStartupState.compare_exchange_strong(expected, AddonStartupState::Starting)) {
        expected = AddonStartupState::Failed;
        if (!g_addonStartupState.compare_exchange_strong(expected, AddonStartupState::Starting)) {
            return 0;
        }
    }

    StartupResetProgress();
    g_startupStartTick.store(GetTickCount64());
    g_startupEndTick.store(0);
    StartupSetStep(StartupStepId::Config, 1, "Preparing startup");
    GUI_SetStatus("Starting Crimson Weather...");
    const bool autoStart = g_nextStartIsAuto.exchange(false);
    Log(autoStart ? "[i] Auto Start requested from config\n" : "[i] Start requested from ReShade overlay\n");

    StartupSetStep(StartupStepId::MinHook, 2, "Initializing hook engine");
    const MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        Log("[E] MH_Initialize failed: %d\n", static_cast<int>(mhStatus));
        MarkStartupFailed("MinHook initialization failed");
        return 0;
    }
    g_minHookInitialized.store(true);

    StartupSetStep(StartupStepId::AobScan, 3, "Scanning game code");
    if (!RunAOBScan()) {
        Log("[E] AOB scan failed\n");
        CleanupFailedStart();
        MarkStartupFailed("AOB scan failed");
        return 0;
    }
    char startupIssue[192] = {};
    if (!RuntimeStartupHealthy(startupIssue, sizeof(startupIssue))) {
        Log("[E] Startup health check failed: %s\n", startupIssue);
        CleanupFailedStart();
        MarkStartupFailed(startupIssue[0] ? startupIssue : "Required hooks unavailable");
        return 0;
    }

#if !defined(CW_WIND_ONLY)
    StartupSetStep(StartupStepId::Presets, 4, "Preparing presets");
    Preset_ArmAutoApplyRemembered();
#else
    StartupSetStep(StartupStepId::Presets, 4, "Wind-only preset step skipped");
#endif
    StartupSetStep(StartupStepId::Hotkeys, 5, "Starting hotkeys");
    if (!StartHotkeyService()) {
        Log("[E] Hotkey service failed to start\n");
        CleanupFailedStart();
        MarkStartupFailed("Hotkey service failed");
        return 0;
    }

    g_initialized.store(true);
    g_addonStartupState.store(AddonStartupState::Ready);
    StartupSetStep(StartupStepId::Ready, 6, "Crimson Weather ready");
    GUI_SetStatus("Ready");
    Log("[+] Ready\n");
    return 0;
}

void OpenStartupLog(HMODULE module) {
    char dir[MAX_PATH] = {};
    ResolveModuleDirectory(module, dir, sizeof(dir));
    LoadConfig(dir);
    OpenLogFile(dir);

    Log("================================================\n");
    Log("  " MOD_DISPLAY_NAME " v" MOD_VERSION "\n");
    Log("================================================\n\n");
    Log("[i] base: %p\n", GetModuleHandle(nullptr));
}

DWORD WINAPI BootstrapThread(void* param) {
    HMODULE module = static_cast<HMODULE>(param);
    if (!IsTargetProcess()) {
        return 0;
    }

    OpenStartupLog(module);
    PrimeMinHookRelayBlock();
    if (g_cfg.autoStart) {
        Log("[i] ReShade addon loaded; Auto Start enabled\n");
        StartupSetStep(StartupStepId::Idle, 0, "Auto Start enabled");
        GUI_SetStatus("Auto Start enabled");
        g_nextStartIsAuto.store(true);
        RequestCrimsonWeatherStart();
    } else {
        Log("[i] ReShade addon loaded; waiting for user start\n");
    }
    return 0;
}

} // namespace

void RequestCrimsonWeatherStart() {
    HANDLE thread = CreateThread(nullptr, 0, &StartThread, nullptr, 0, nullptr);
    if (!thread) {
        MarkStartupFailed("Failed to create startup thread");
        return;
    }
    CloseHandle(thread);
}

bool InitializeCrimsonWeather(HMODULE module) {
    if (!IsTargetProcess()) {
        return true;
    }

    StartupResetProgress();
    if (!InitializeOverlayBridge(module)) {
        MarkStartupFailed("ReShade addon registration failed");
        return false;
    }
    InitializeSkyTextureOverride(module);
    g_addonStartupState.store(AddonStartupState::NotStarted);
    StartupSetStep(StartupStepId::Idle, 0, "Click Start to initialize");
    GUI_SetStatus("Click Start to initialize");

    HANDLE thread = CreateThread(nullptr, 0, &BootstrapThread, module, 0, nullptr);
    if (!thread) {
        MarkStartupFailed("Failed to create bootstrap thread");
        return true;
    }
    CloseHandle(thread);
    return true;
}

void ShutdownCrimsonWeather() {
    if (!g_initialized.exchange(false)) {
        RestoreRuntimePatches();
        StopHotkeyService();
        ShutdownSkyTextureOverride();
        ShutdownOverlayBridge();
        if (g_minHookInitialized.exchange(false)) {
            MH_Uninitialize();
        }
        return;
    }

    SuspendTimeControl();
    RestoreRuntimePatches();
    StopHotkeyService();
    ShutdownSkyTextureOverride();
    ShutdownOverlayBridge();
    if (g_minHookInitialized.exchange(false)) {
        MH_Uninitialize();
    }
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}
