#include "pch.h"

bool InitializeCrimsonWeather(HMODULE module, HMODULE reshadeModule);
void ShutdownCrimsonWeather(HMODULE module, HMODULE reshadeModule);
void CrimsonWeather_DllDetachSignal() noexcept;

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addonModule, HMODULE reshadeModule) {
    return InitializeCrimsonWeather(addonModule, reshadeModule);
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addonModule, HMODULE reshadeModule) {
    ShutdownCrimsonWeather(addonModule, reshadeModule);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        // No waits, locks, API unregister calls, or C++ teardown under loader lock.
        CrimsonWeather_DllDetachSignal();
    }
    return TRUE;
}
