#include "pch.h"

bool InitializeCrimsonWeather(HMODULE module);
void ShutdownCrimsonWeather();

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        InitializeCrimsonWeather(hModule);
    } else if (reason == DLL_PROCESS_DETACH) {
        ShutdownCrimsonWeather();
    }
    return TRUE;
}

