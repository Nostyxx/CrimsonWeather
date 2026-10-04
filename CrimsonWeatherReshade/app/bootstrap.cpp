#include "pch.h"

#include "sky_texture_override.h"
#include "overlay_bridge.h"
#include "community_service.h"
#include "preset_service.h"
#include "runtime_shared.h"
#include "update_service.h"

#include <memory>
#include <mutex>
#include <new>

extern "C" BOOL ReserveBufferBlock(LPVOID pOrigin);

namespace {

std::atomic<bool> g_initialized{ false };
std::atomic<bool> g_minHookInitialized{ false };
std::atomic<bool> g_nextStartIsAuto{ false };
std::atomic<bool> g_shutdownRequested{ false };
std::mutex g_trackedThreadMutex;
HANDLE g_bootstrapThread = nullptr;
HANDLE g_startThread = nullptr;
HMODULE g_addonLifetimePin = nullptr;
HMODULE g_reshadeModule = nullptr;
constexpr DWORD kStartupThreadWaitMs = 300000;

enum class TrackedThreadStartResult {
    Started,
    AlreadyRunning,
    ShuttingDown,
    Failed,
};

struct TrackedThreadContext {
    LPTHREAD_START_ROUTINE procedure = nullptr;
    void* parameter = nullptr;
    HMODULE moduleReference = nullptr;
};

DWORD WINAPI TrackedThreadEntry(void* rawContext) {
    std::unique_ptr<TrackedThreadContext> context(static_cast<TrackedThreadContext*>(rawContext));
    DWORD result = 1;
    try {
        result = context->procedure(context->parameter);
    } catch (...) {
        Log("[E] startup worker failed unexpectedly\n");
    }
    HMODULE moduleReference = context->moduleReference;
    context.reset();
    if (moduleReference) {
        FreeLibraryAndExitThread(moduleReference, result);
    }
    return result;
}

TrackedThreadStartResult StartTrackedThread(
    HANDLE& threadSlot,
    LPTHREAD_START_ROUTINE procedure,
    void* parameter) {
    std::lock_guard<std::mutex> lock(g_trackedThreadMutex);
    if (g_shutdownRequested.load()) return TrackedThreadStartResult::ShuttingDown;
    if (threadSlot) {
        if (WaitForSingleObject(threadSlot, 0) != WAIT_OBJECT_0) {
            return TrackedThreadStartResult::AlreadyRunning;
        }
        CloseHandle(threadSlot);
        threadSlot = nullptr;
    }

    HMODULE moduleReference = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(procedure),
            &moduleReference)) {
        return TrackedThreadStartResult::Failed;
    }
    auto* context = new (std::nothrow) TrackedThreadContext{ procedure, parameter, moduleReference };
    if (!context) {
        FreeLibrary(moduleReference);
        return TrackedThreadStartResult::Failed;
    }
    HANDLE thread = CreateThread(nullptr, 0, &TrackedThreadEntry, context, 0, nullptr);
    if (!thread) {
        delete context;
        FreeLibrary(moduleReference);
        return TrackedThreadStartResult::Failed;
    }
    threadSlot = thread;
    return TrackedThreadStartResult::Started;
}

bool WaitForTrackedThread(HANDLE& threadSlot, DWORD waitMilliseconds) {
    HANDLE thread = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_trackedThreadMutex);
        thread = threadSlot;
    }
    if (!thread) return true;
    const DWORD result = WaitForSingleObject(thread, waitMilliseconds);
    if (result != WAIT_OBJECT_0) return false;
    std::lock_guard<std::mutex> lock(g_trackedThreadMutex);
    if (threadSlot == thread) {
        CloseHandle(threadSlot);
        threadSlot = nullptr;
    }
    return true;
}

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
    if (g_shutdownRequested.load()) return;
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
        ? "[startup] minhook: relay block reserved near game base\n"
        : "[W] MinHook relay block reserve failed near game base\n");
}

bool TryInitializeSkyTextureOverride(HMODULE module) {
    __try {
        return InitializeSkyTextureOverride(module);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        const DWORD code = GetExceptionCode();
        Log("[E] Sky texture override crashed during init: 0x%08lX; continuing without moon/milkyway texture switching\n", code);
        GUI_SetStatus("Moon/Milky Way texture switching unavailable");
        return false;
    }
}

DWORD WINAPI StartThread(void*) {
    if (g_shutdownRequested.load()) return 0;
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
    Log("[startup] runtime: start requested source=%s\n", autoStart ? "auto" : "overlay");

#if defined(CW_DEV_BUILD)
    const DevLaunchOption launchOption = g_devLaunchOption.load();
    if (!DevLaunchOptionUsesRuntimeStartup(launchOption)) {
        StartupSetStep(StartupStepId::Ready, 6, DevLaunchOptionDescription(launchOption));
        g_initialized.store(true);
        g_addonStartupState.store(AddonStartupState::Ready);
        GUI_SetStatus(DevLaunchOptionDescription(launchOption));
        Log("[dev] Runtime startup skipped for LaunchOption=%s\n", DevLaunchOptionName(launchOption));
        return 0;
    }
#endif

    StartupSetStep(StartupStepId::MinHook, 2, "Initializing hook engine");
    Log("[startup] runtime: initializing hook engine\n");
    const MH_STATUS mhStatus = MH_Initialize();
    if (mhStatus != MH_OK && mhStatus != MH_ERROR_ALREADY_INITIALIZED) {
        Log("[E] MH_Initialize failed: %d\n", static_cast<int>(mhStatus));
        MarkStartupFailed("MinHook initialization failed");
        return 0;
    }
    g_minHookInitialized.store(true);

    StartupSetStep(StartupStepId::AobScan, 3, "Scanning game code");
    Log("[startup] runtime: scanning game code\n");
    if (!RunAOBScan()) {
        if (g_shutdownRequested.load()) {
            CleanupFailedStart();
            return 0;
        }
        Log("[E] AOB scan failed\n");
        CleanupFailedStart();
        MarkStartupFailed("AOB scan failed");
        return 0;
    }
    if (g_shutdownRequested.load()) {
        CleanupFailedStart();
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
    if (g_cfg.realGameTimeEnabled) {
        if (RuntimeHookEnabled(RuntimeHookId::GameTimeGetter)) {
            g_realGameTimeEnabled.store(true);
            g_realGameTimeSetMinuteRequest.store(-1);
            g_realGameTimeDayDeltaRequest.store(0);
            g_timeApplyRequest.store(true);
            Log("[startup] real-time: restored enabled dayScale=x%.4f nightScale=x%.4f\n",
                g_realGameTimeDayScale.load(),
                g_realGameTimeNightScale.load());
        } else {
            Log("[W] Real In-Game Time was enabled in config but GameTimeGetter is unavailable; activation skipped\n");
        }
    }

    StartupSetStep(StartupStepId::Presets, 4, "Preparing presets");
    Log("[startup] runtime: preparing presets\n");
    Preset_ArmAutoApplyRemembered();
#else
    StartupSetStep(StartupStepId::Presets, 4, "Wind-only preset step skipped");
    Log("[startup] runtime: preset step skipped for Wind-only build\n");
#endif
    StartupSetStep(StartupStepId::Hotkeys, 5, "Starting hotkeys");
    Log("[startup] runtime: starting hotkeys\n");
    if (!StartHotkeyService()) {
        Log("[E] Hotkey service failed to start\n");
        CleanupFailedStart();
        MarkStartupFailed("Hotkey service failed");
        return 0;
    }

    if (g_shutdownRequested.load()) {
        CleanupFailedStart();
        return 0;
    }

    g_initialized.store(true);
    g_addonStartupState.store(AddonStartupState::Ready);
    StartupSetStep(StartupStepId::Ready, 6, "Crimson Weather ready");
    GUI_SetStatus("Ready");
    Log("[startup] runtime: ready\n");
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
    Log("[startup] bootstrap: base=%p\n", GetModuleHandle(nullptr));
}

DWORD WINAPI BootstrapThread(void* param) {
    HMODULE module = static_cast<HMODULE>(param);
    if (!IsTargetProcess() || g_shutdownRequested.load()) {
        return 0;
    }

    OpenStartupLog(module);
    UpdateService_CleanupStaleFiles();
#if defined(CW_DEV_BUILD)
    const DevLaunchOption launchOption = g_devLaunchOption.load();
    const bool useTextureHook = g_cfg.textureSwitcherEnabled && DevLaunchOptionUsesTextureHook(launchOption);
    const bool mayUseRuntimeHooks = DevLaunchOptionUsesRuntimeStartup(launchOption);
    Log("[dev] Bootstrap LaunchOption=%s (%s)\n",
        DevLaunchOptionName(launchOption),
        DevLaunchOptionDescription(launchOption));
    if (useTextureHook || mayUseRuntimeHooks) {
        PrimeMinHookRelayBlock();
    } else {
        Log("[dev] MinHook relay prime skipped for LaunchOption=%s\n", DevLaunchOptionName(launchOption));
    }
    if (useTextureHook) {
        Log("[startup] textures: initializing moon/milkyway switcher\n");
        const bool skyTextureOk = TryInitializeSkyTextureOverride(module);
        Log(skyTextureOk ? "[startup] textures: ready\n" : "[W] Sky texture override disabled\n");
    } else if (!g_cfg.textureSwitcherEnabled) {
        Log("[i] Sky texture override skipped: TextureSwitcher.Enabled=0\n");
    } else {
        Log("[dev] Sky texture override skipped for LaunchOption=%s\n", DevLaunchOptionName(launchOption));
    }
#else
    PrimeMinHookRelayBlock();
    if (g_cfg.textureSwitcherEnabled) {
        Log("[startup] textures: initializing moon/milkyway switcher\n");
        const bool skyTextureOk = TryInitializeSkyTextureOverride(module);
        Log(skyTextureOk ? "[startup] textures: ready\n" : "[W] Sky texture override disabled\n");
    } else {
        Log("[i] Sky texture override skipped: TextureSwitcher.Enabled=0\n");
    }
#endif
    if (g_shutdownRequested.load()) return 0;
    if (g_cfg.autoStart) {
        Log("[startup] addon: loaded autoStart=1\n");
        StartupSetStep(StartupStepId::Idle, 0, "Auto Start enabled");
        GUI_SetStatus("Auto Start enabled");
        g_nextStartIsAuto.store(true);
        RequestCrimsonWeatherStart();
    } else {
        Log("[startup] addon: loaded autoStart=0 waiting for user start\n");
    }
    return 0;
}

} // namespace

bool CrimsonWeatherShutdownRequested() {
    return g_shutdownRequested.load();
}

void RequestCrimsonWeatherStart() {
    if (g_shutdownRequested.load()) return;
    const TrackedThreadStartResult result = StartTrackedThread(g_startThread, &StartThread, nullptr);
    if (result == TrackedThreadStartResult::Failed) {
        MarkStartupFailed("Failed to create startup thread");
    }
}

bool InitializeCrimsonWeather(HMODULE module, HMODULE reshadeModule) {
    if (!IsTargetProcess()) {
        return true;
    }
    if (g_shutdownRequested.load()) return false;

    HMODULE lifetimePin = nullptr;
    if (!GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&InitializeCrimsonWeather),
            &lifetimePin)) {
        return false;
    }
    g_addonLifetimePin = lifetimePin;
    g_reshadeModule = reshadeModule;

    StartupResetProgress();
    if (!InitializeOverlayBridge(module, reshadeModule)) {
        MarkStartupFailed("ReShade addon registration failed");
        g_addonLifetimePin = nullptr;
        FreeLibrary(lifetimePin);
        return false;
    }
    g_addonStartupState.store(AddonStartupState::NotStarted);
    StartupSetStep(StartupStepId::Idle, 0, "Click Start to initialize");
    GUI_SetStatus("Click Start to initialize");

    const TrackedThreadStartResult result = StartTrackedThread(g_bootstrapThread, &BootstrapThread, module);
    if (result == TrackedThreadStartResult::Failed) {
        MarkStartupFailed("Failed to create bootstrap thread");
    }
    return true;
}

void ShutdownCrimsonWeather(HMODULE module, HMODULE reshadeModule) {
    {
        std::lock_guard<std::mutex> lock(g_trackedThreadMutex);
        if (g_shutdownRequested.exchange(true)) return;
    }

    Community_BeginShutdown();
    UpdateService_BeginShutdown();
    ShutdownOverlayBridge(module, reshadeModule ? reshadeModule : g_reshadeModule);

    const ULONGLONG deadline = GetTickCount64() + kStartupThreadWaitMs;
    const auto remainingWait = [&]() -> DWORD {
        const ULONGLONG now = GetTickCount64();
        return now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
    };
    const bool startupStopped =
        WaitForTrackedThread(g_startThread, remainingWait()) &&
        WaitForTrackedThread(g_bootstrapThread, remainingWait());
    const bool communityStopped = Community_WaitForShutdown(remainingWait());
    const bool updaterStopped = UpdateService_WaitForShutdown(remainingWait());
    if (!startupStopped || !communityStopped || !updaterStopped) {
        Log("[W] unload stop exceeded %lu ms; retaining the add-on module reference to keep active code mapped\n",
            kStartupThreadWaitMs);
        return;
    }

    Community_CloseAfterShutdown();
    UpdateService_CloseAfterShutdown();

    if (g_initialized.exchange(false)) {
        SuspendTimeControl();
    }
    RestoreRuntimePatches();
    StopHotkeyService();
    ShutdownSkyTextureOverride();
    if (g_minHookInitialized.exchange(false)) {
        MH_Uninitialize();
    }
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
#if defined(CW_DEV_BUILD)
    if (g_devLaunchLogFile) {
        fclose(g_devLaunchLogFile);
        g_devLaunchLogFile = nullptr;
    }
#endif

    HMODULE lifetimePin = g_addonLifetimePin;
    g_addonLifetimePin = nullptr;
    g_reshadeModule = nullptr;
    if (lifetimePin) FreeLibrary(lifetimePin);
}

void CrimsonWeather_DllDetachSignal() noexcept {
    if (g_shutdownRequested.exchange(true)) return;
    Community_SignalStopWithoutWait();
    UpdateService_SignalStopWithoutWait();
}
