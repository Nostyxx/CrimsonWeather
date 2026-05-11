#include "pch.h"

#include "runtime_shared.h"

namespace {

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);

std::atomic<bool> g_hotkeyServiceRunning{ false };
HANDLE g_hotkeyStopEvent = nullptr;
HANDLE g_hotkeyThread = nullptr;
HMODULE g_xinputModule = nullptr;
XInputGetStateFn g_xinputGetState = nullptr;

struct XInputGamepad {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct XInputStateRaw {
    DWORD dwPacketNumber;
    XInputGamepad Gamepad;
};

bool ResolveXInput() {
    if (g_xinputGetState) {
        return true;
    }

    static const char* kCandidates[] = {
        "xinput1_4.dll",
        "xinput9_1_0.dll",
        "xinput1_3.dll",
    };

    for (const char* dll : kCandidates) {
        HMODULE module = LoadLibraryA(dll);
        if (!module) {
            continue;
        }

        auto fn = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
        if (fn) {
            g_xinputModule = module;
            g_xinputGetState = fn;
            return true;
        }
        FreeLibrary(module);
    }

    return false;
}

DWORD ReadControllerButtons() {
    if (!ResolveXInput()) {
        return 0;
    }

    XInputStateRaw state{};
    if (g_xinputGetState(0, &state) != ERROR_SUCCESS) {
        return 0;
    }
    return state.Gamepad.wButtons;
}

DWORD WINAPI HotkeyThreadProc(void*) {
    bool keyboardWasDown = false;
    bool controllerWasDown = false;

    while (WaitForSingleObject(g_hotkeyStopEvent, 16) == WAIT_TIMEOUT) {
        const bool keyboardDown = (GetAsyncKeyState(g_cfg.effectToggleVK) & 0x8000) != 0;
        if (keyboardDown && !keyboardWasDown) {
            ToggleModEnabled();
            Log("[hotkey] effect toggle pressed (keyboard)\n");
        }
        keyboardWasDown = keyboardDown;

        const WORD buttons = static_cast<WORD>(ReadControllerButtons());
        const bool controllerDown = IsControllerComboPressed(buttons, g_cfg.controllerEffectToggleMask);
        if (controllerDown && !controllerWasDown) {
            ToggleModEnabled();
            Log("[hotkey] effect toggle pressed (controller)\n");
        }
        controllerWasDown = controllerDown;
    }

    return 0;
}

} // namespace

bool StartHotkeyService() {
    if (g_hotkeyServiceRunning.load()) {
        return true;
    }

    g_hotkeyStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_hotkeyStopEvent) {
        return false;
    }

    g_hotkeyThread = CreateThread(nullptr, 0, &HotkeyThreadProc, nullptr, 0, nullptr);
    if (!g_hotkeyThread) {
        CloseHandle(g_hotkeyStopEvent);
        g_hotkeyStopEvent = nullptr;
        return false;
    }

    g_hotkeyServiceRunning.store(true);
    return true;
}

void StopHotkeyService() {
    if (!g_hotkeyServiceRunning.exchange(false)) {
        return;
    }

    if (g_hotkeyStopEvent) {
        SetEvent(g_hotkeyStopEvent);
    }
    if (g_hotkeyThread) {
        const DWORD waitResult = WaitForSingleObject(g_hotkeyThread, 500);
        if (waitResult == WAIT_TIMEOUT) {
            Log("[W] hotkey thread did not stop within 500ms\n");
        }
        CloseHandle(g_hotkeyThread);
        g_hotkeyThread = nullptr;
    }
    if (g_hotkeyStopEvent) {
        CloseHandle(g_hotkeyStopEvent);
        g_hotkeyStopEvent = nullptr;
    }
    if (g_xinputModule) {
        FreeLibrary(g_xinputModule);
        g_xinputModule = nullptr;
        g_xinputGetState = nullptr;
    }
}
