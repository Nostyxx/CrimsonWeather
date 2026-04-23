#include "pch.h"

#include "overlay_bridge.h"
#include "preset_service.h"
#include "runtime_shared.h"

namespace {

std::atomic<bool> g_initialized{ false };

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

DWORD WINAPI BootstrapThread(void* param) {
    HMODULE module = static_cast<HMODULE>(param);
    if (!IsTargetProcess()) {
        return 0;
    }

    char dir[MAX_PATH] = {};
    ResolveModuleDirectory(module, dir, sizeof(dir));
    LoadConfig(dir);
    OpenLogFile(dir);

    Log("================================================\n");
    Log("  " MOD_NAME " v" MOD_VERSION "\n");
    Log("================================================\n\n");
    Log("[i] base: %p\n", GetModuleHandle(nullptr));

    if (!InitializeOverlayBridge(module)) {
        Log("[E] ReShade addon registration failed; aborting initialization\n");
        GUI_SetStatus("ReShade addon registration failed");
        return 0;
    }

    if (MH_Initialize() != MH_OK) {
        Log("[E] MH_Initialize failed\n");
        GUI_SetStatus("MinHook initialization failed");
        ShutdownOverlayBridge();
        return 0;
    }

    if (!RunAOBScan()) {
        Log("[E] AOB scan failed\n");
        GUI_SetStatus("AOB scan failed");
        MH_Uninitialize();
        ShutdownOverlayBridge();
        return 0;
    }

    Preset_ArmAutoApplyRemembered();
    if (!StartHotkeyService()) {
        Log("[E] Hotkey service failed to start\n");
        GUI_SetStatus("Hotkey service failed");
        MH_Uninitialize();
        ShutdownOverlayBridge();
        return 0;
    }
    g_initialized.store(true);
    GUI_SetStatus("Ready");
    Log("[+] Ready\n");
    return 0;
}

} // namespace

bool InitializeCrimsonWeather(HMODULE module) {
    HANDLE thread = CreateThread(nullptr, 0, &BootstrapThread, module, 0, nullptr);
    if (!thread) {
        return false;
    }
    CloseHandle(thread);
    return true;
}

void ShutdownCrimsonWeather() {
    if (!g_initialized.exchange(false)) {
        RestoreRuntimePatches();
        StopHotkeyService();
        ShutdownOverlayBridge();
        return;
    }

    SuspendTimeControl();
    RestoreRuntimePatches();
    StopHotkeyService();
    ShutdownOverlayBridge();
    MH_Uninitialize();
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}
