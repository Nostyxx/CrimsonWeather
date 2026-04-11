#include "pch.h"
#include "cw_shared.h"
#include "dxgi_hook_backend.h"
#include "preset_system.h"
#include <GameInput.h>
#include <hidsdi.h>
#include <hidusage.h>
#include <string>
#include <vector>

#pragma comment(lib, "hid.lib")

#define CW_LOG_ONCE(flag_name, ...) \
    do { \
        static bool flag_name = false; \
        if (!flag_name) { \
            flag_name = true; \
            Log(__VA_ARGS__); \
        } \
    } while (0)

// Dear ImGui DX12 overlay
static IDXGISwapChain3* g_lastOverlayRenderedSwapChain = nullptr;
static UINT g_lastOverlayRenderedBufferIndex = UINT_MAX;
static ULONGLONG g_lastOverlayRenderTick = 0;
static bool g_overlayFenceTimeoutWarned = false;
static bool g_activeGpuLogged = false;
static bool g_overlayFirstAttemptLogged = false;
static bool g_overlayFirstRecordFailureLogged = false;
static bool g_overlayFirstSubmissionLogged = false;
static SRWLOCK g_dx12QueueLock = SRWLOCK_INIT;
static bool g_dx12QueueFrozen = false;
static bool g_dx12QueueFreezeLogged = false;
using ExecuteCommandLists_fn = void (STDMETHODCALLTYPE*)(ID3D12CommandQueue* self, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists);
static ExecuteCommandLists_fn g_pOrigExecuteCommandLists = nullptr;
static bool g_d3d12QueueHookInstalled = false;
static HWND g_dualSenseRawInputHwnd = nullptr;
static bool g_dualSenseFirstPacketLogged = false;
static bool g_dualSenseFirstActivePacketLogged = false;
static bool g_dualSenseSuppressedByXInputLogged = false;
static bool g_xinputFirstPacketLogged = false;
static DWORD g_lastUnsupportedSonyPidLogged = 0;
static std::atomic<ULONGLONG> g_lastXInputSeenTick{ 0 };
static std::atomic<ULONGLONG> g_lastDualSenseRawPacketTick{ 0 };
static HMODULE g_gameInputModule = nullptr;
static IGameInput* g_gameInput = nullptr;
static bool g_gameInputInitAttempted = false;
static bool g_gameInputReadyLogged = false;
static bool g_gameInputUnavailableLogged = false;
static bool g_gameInputFirstDeviceLogged = false;
static bool g_gameInputFirstActiveLogged = false;
using GameInputCreate_fn = HRESULT(STDAPICALLTYPE*)(IGameInput** gameInput);
static GameInputCreate_fn g_pGameInputCreate = nullptr;

struct CachedOverlayDrawData {
    ImDrawData drawData;
    ImVector<ImDrawList*> drawLists;
    bool valid = false;
};

static CachedOverlayDrawData g_cachedOverlayDrawData = {};

static void DrawImGuiMenu();
static void ClearCachedOverlayDrawData();
static void ShutdownImGuiRuntime(bool restoreWndProc);
static void ResetControllerRepeatState();

static void RegisterDualSenseRawInput(HWND hwnd);
static bool IsXInputHidShim(HANDLE hDevice);
static bool HasSupportedSonyRawInputDevice();
static bool PollGameInputButtons(WORD* outButtons);

static WORD MapGameInputButtons(GameInputGamepadButtons buttons) {
    WORD mapped = 0;
    if ((buttons & GameInputGamepadMenu) != 0) mapped |= XINPUT_GAMEPAD_START;
    if ((buttons & GameInputGamepadView) != 0) mapped |= XINPUT_GAMEPAD_BACK;
    if ((buttons & GameInputGamepadA) != 0) mapped |= XINPUT_GAMEPAD_A;
    if ((buttons & GameInputGamepadB) != 0) mapped |= XINPUT_GAMEPAD_B;
    if ((buttons & GameInputGamepadX) != 0) mapped |= XINPUT_GAMEPAD_X;
    if ((buttons & GameInputGamepadY) != 0) mapped |= XINPUT_GAMEPAD_Y;
    if ((buttons & GameInputGamepadDPadUp) != 0) mapped |= XINPUT_GAMEPAD_DPAD_UP;
    if ((buttons & GameInputGamepadDPadDown) != 0) mapped |= XINPUT_GAMEPAD_DPAD_DOWN;
    if ((buttons & GameInputGamepadDPadLeft) != 0) mapped |= XINPUT_GAMEPAD_DPAD_LEFT;
    if ((buttons & GameInputGamepadDPadRight) != 0) mapped |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if ((buttons & GameInputGamepadLeftShoulder) != 0) mapped |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if ((buttons & GameInputGamepadRightShoulder) != 0) mapped |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if ((buttons & GameInputGamepadLeftThumbstick) != 0) mapped |= XINPUT_GAMEPAD_LEFT_THUMB;
    if ((buttons & GameInputGamepadRightThumbstick) != 0) mapped |= XINPUT_GAMEPAD_RIGHT_THUMB;
    return mapped;
}

static bool EnsureGameInput() {
    if (g_gameInput) return true;
    if (g_gameInputInitAttempted) return false;
    g_gameInputInitAttempted = true;

    g_gameInputModule = LoadLibraryA("GameInput.dll");
    if (!g_gameInputModule) {
        if (!g_gameInputUnavailableLogged) {
            g_gameInputUnavailableLogged = true;
            Log("[input] GameInput.dll not available; controller fallback disabled\n");
        }
        return false;
    }

    g_pGameInputCreate = reinterpret_cast<GameInputCreate_fn>(GetProcAddress(g_gameInputModule, "GameInputCreate"));
    if (!g_pGameInputCreate) {
        if (!g_gameInputUnavailableLogged) {
            g_gameInputUnavailableLogged = true;
            Log("[input] GameInputCreate export not found; controller fallback disabled\n");
        }
        return false;
    }

    HRESULT hr = g_pGameInputCreate(&g_gameInput);
    if (FAILED(hr) || !g_gameInput) {
        if (!g_gameInputUnavailableLogged) {
            g_gameInputUnavailableLogged = true;
            Log("[input] GameInputCreate failed hr=0x%08X; controller fallback disabled\n", static_cast<unsigned>(hr));
        }
        return false;
    }

    if (!g_gameInputReadyLogged) {
        g_gameInputReadyLogged = true;
        Log("[input] GameInput initialized for non-XInput controller fallback\n");
    }
    return true;
}

static bool PollGameInputButtons(WORD* outButtons) {
    if (outButtons) *outButtons = 0;
    if (!EnsureGameInput()) return false;

    IGameInputReading* reading = nullptr;
    HRESULT hr = g_gameInput->GetCurrentReading(GameInputKindGamepad, nullptr, &reading);
    if (FAILED(hr) || !reading) return false;

    IGameInputDevice* device = nullptr;
    GameInputDeviceInfo const* info = nullptr;
    reading->GetDevice(&device);
    if (device) {
        info = device->GetDeviceInfo();
        if (info && !g_gameInputFirstDeviceLogged) {
            g_gameInputFirstDeviceLogged = true;
            const char* displayName = (info->displayName && info->displayName->data) ? info->displayName->data : "<unnamed>";
            Log("[input] GameInput device vid=0x%04X pid=0x%04X family=%d supported=0x%08X name=%s\n",
                info->vendorId, info->productId, static_cast<int>(info->deviceFamily),
                static_cast<unsigned>(info->supportedInput), displayName);
        }
    }

    GameInputGamepadState state{};
    const bool ok = reading->GetGamepadState(&state);
    if (ok) {
        const WORD buttons = MapGameInputButtons(state.buttons);
        g_dualSenseButtons = buttons;
        g_dualSenseLastInputMs = GetTickCount64();
        if (outButtons) *outButtons = buttons;
        if (buttons != 0 && !g_gameInputFirstActiveLogged) {
            g_gameInputFirstActiveLogged = true;
            if (info) {
                Log("[input] GameInput active buttons=0x%04X vid=0x%04X pid=0x%04X\n",
                    buttons, info->vendorId, info->productId);
            } else {
                Log("[input] GameInput active buttons=0x%04X\n", buttons);
            }
        }
    }

    if (device) device->Release();
    reading->Release();
    return ok;
}

static void LogRawInputHidDevice(HANDLE hDevice, const char* context) {
    if (!hDevice) return;

    RID_DEVICE_INFO info{};
    info.cbSize = sizeof(info);
    UINT infoSize = sizeof(info);
    if (GetRawInputDeviceInfoA(hDevice, RIDI_DEVICEINFO, &info, &infoSize) == static_cast<UINT>(-1))
        return;

    char nameBuf[512] = {};
    UINT nameSize = sizeof(nameBuf);
    if (GetRawInputDeviceInfoA(hDevice, RIDI_DEVICENAME, nameBuf, &nameSize) == static_cast<UINT>(-1)) {
        nameBuf[0] = '\0';
    }

    if (info.dwType == RIM_TYPEHID) {
        Log("[input] %s hid=%p vid=0x%04X pid=0x%04X usagePage=0x%04X usage=0x%04X name=%s\n",
            context ? context : "hid",
            hDevice,
            info.hid.dwVendorId,
            info.hid.dwProductId,
            info.hid.usUsagePage,
            info.hid.usUsage,
            nameBuf[0] ? nameBuf : "<unknown>");
    }
}

static bool IsXInputHidShim(HANDLE hDevice) {
    if (!hDevice) return false;

    UINT nameSize = 0;
    if (GetRawInputDeviceInfoA(hDevice, RIDI_DEVICENAME, nullptr, &nameSize) != 0 || nameSize == 0)
        return false;

    std::string name(nameSize, '\0');
    if (GetRawInputDeviceInfoA(hDevice, RIDI_DEVICENAME, name.data(), &nameSize) == static_cast<UINT>(-1))
        return false;

    return name.find("IG_") != std::string::npos;
}

static ID3D12CommandQueue* AcquireOverlayQueueRef() {
    ID3D12CommandQueue* queue = nullptr;
    AcquireSRWLockShared(&g_dx12QueueLock);
    queue = g_dx12Queue;
    if (queue) queue->AddRef();
    ReleaseSRWLockShared(&g_dx12QueueLock);
    return queue;
}

static void FreezeOverlayQueueCapture(ID3D12CommandQueue* pinnedQueue = nullptr) {
    AcquireSRWLockExclusive(&g_dx12QueueLock);
    if (pinnedQueue && g_dx12Queue != pinnedQueue) {
        if (g_dx12Queue) {
            g_dx12Queue->Release();
        }
        g_dx12Queue = pinnedQueue;
        g_dx12Queue->AddRef();
    }
    g_dx12QueueFrozen = true;
    ReleaseSRWLockExclusive(&g_dx12QueueLock);
}

static void UnfreezeOverlayQueueCapture() {
    AcquireSRWLockExclusive(&g_dx12QueueLock);
    g_dx12QueueFrozen = false;
    g_dx12QueueFreezeLogged = false;
    ReleaseSRWLockExclusive(&g_dx12QueueLock);
}

static bool IsModuleCurrentlyLoaded(const char* moduleName) {
    return moduleName && GetModuleHandleA(moduleName) != nullptr;
}

static void ResetOverlayOpenDiagnostics() {
    g_overlayFirstAttemptLogged = false;
    g_overlayFirstRecordFailureLogged = false;
    g_overlayFirstSubmissionLogged = false;
}

static void LogOverlayOpenSnapshot(const char* source) {
    ID3D12CommandQueue* queue = AcquireOverlayQueueRef();
    Log("[overlay] menu open via %s imgui=%d queue=%p owner_sc=%p amd_fg=%d dlssg=%d streamline=%d xefg=%d\n",
        source ? source : "unknown",
        g_imguiReady ? 1 : 0,
        (void*)queue,
        (void*)g_swapChain3,
        IsModuleCurrentlyLoaded("amd_fidelityfx_framegeneration_dx12.dll") ? 1 : 0,
        IsModuleCurrentlyLoaded("nvngx_dlssg.dll") ? 1 : 0,
        IsModuleCurrentlyLoaded("sl.interposer.dll") ? 1 : 0,
        IsModuleCurrentlyLoaded("libxess_fg.dll") ? 1 : 0);
    if (queue) queue->Release();
}

static void ResetControllerRepeatState() {
    g_padUiPollPrevButtons = 0;
    g_padHoldNextMs[0] = g_padHoldNextMs[1] = g_padHoldNextMs[2] = g_padHoldNextMs[3] = 0;
    g_padHoldStartMs[0] = g_padHoldStartMs[1] = g_padHoldStartMs[2] = g_padHoldStartMs[3] = 0;
}

static bool IsDualSenseHidDevice(HANDLE hDevice) {
    if (!hDevice) return false;
    if (IsXInputHidShim(hDevice)) return false;

    RID_DEVICE_INFO info{};
    info.cbSize = sizeof(info);
    UINT size = sizeof(info);
    if (GetRawInputDeviceInfoA(hDevice, RIDI_DEVICEINFO, &info, &size) == static_cast<UINT>(-1))
        return false;
    if (info.dwType != RIM_TYPEHID) return false;
    if (info.hid.dwVendorId != 0x054C) return false;
    const bool supported = info.hid.dwProductId == 0x0CE6 || info.hid.dwProductId == 0x0DF2;
    if (!supported && g_lastUnsupportedSonyPidLogged != info.hid.dwProductId) {
        g_lastUnsupportedSonyPidLogged = info.hid.dwProductId;
        Log("[W] Sony HID device detected but PID 0x%04X is not in native DualSense support list\n",
            info.hid.dwProductId);
        LogRawInputHidDevice(hDevice, "sony-unsupported");
    }
    return supported;
}

static bool HasSupportedSonyRawInputDevice() {
    UINT deviceCount = 0;
    if (GetRawInputDeviceList(nullptr, &deviceCount, sizeof(RAWINPUTDEVICELIST)) != 0 || deviceCount == 0)
        return false;

    std::vector<RAWINPUTDEVICELIST> devices(deviceCount);
    if (GetRawInputDeviceList(devices.data(), &deviceCount, sizeof(RAWINPUTDEVICELIST)) == static_cast<UINT>(-1))
        return false;

    for (UINT i = 0; i < deviceCount; ++i) {
        const HANDLE hDevice = devices[i].hDevice;
        if (IsXInputHidShim(hDevice)) continue;
        if (IsDualSenseHidDevice(hDevice)) {
            LogRawInputHidDevice(hDevice, "sony-supported-present");
            return true;
        }
    }

    return false;
}

static bool GetDualSensePreparsedData(HANDLE hDevice, std::vector<BYTE>& storage, PHIDP_PREPARSED_DATA* outData) {
    if (outData) *outData = nullptr;
    UINT size = 0;
    if (GetRawInputDeviceInfoA(hDevice, RIDI_PREPARSEDDATA, nullptr, &size) == static_cast<UINT>(-1) || size == 0)
        return false;
    storage.resize(size);
    if (GetRawInputDeviceInfoA(hDevice, RIDI_PREPARSEDDATA, storage.data(), &size) == static_cast<UINT>(-1))
        return false;
    if (outData) *outData = reinterpret_cast<PHIDP_PREPARSED_DATA>(storage.data());
    return true;
}

static WORD MapDualSenseReportToXInputButtons(HANDLE hDevice, const BYTE* report, ULONG reportLen) {
    if (!hDevice || !report || reportLen == 0) return 0;

    std::vector<BYTE> preparsedStorage;
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!GetDualSensePreparsedData(hDevice, preparsedStorage, &preparsed) || !preparsed)
        return 0;

    WORD buttons = 0;
    USAGE usages[32] = {};
    ULONG usageCount = static_cast<ULONG>(std::size(usages));
    if (HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, usages, &usageCount,
        preparsed, reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), reportLen) == HIDP_STATUS_SUCCESS) {
        for (ULONG i = 0; i < usageCount; ++i) {
            switch (usages[i]) {
            case 1: buttons |= XINPUT_GAMEPAD_X; break;
            case 2: buttons |= XINPUT_GAMEPAD_A; break;
            case 3: buttons |= XINPUT_GAMEPAD_B; break;
            case 4: buttons |= XINPUT_GAMEPAD_Y; break;
            case 5: buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER; break;
            case 6: buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; break;
            default: break;
            }
        }
    }

    ULONG hatValue = 8;
    if (HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_HATSWITCH,
        &hatValue, preparsed, reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), reportLen) == HIDP_STATUS_SUCCESS) {
        switch (hatValue) {
        case 0: buttons |= XINPUT_GAMEPAD_DPAD_UP; break;
        case 1: buttons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT; break;
        case 2: buttons |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
        case 3: buttons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT; break;
        case 4: buttons |= XINPUT_GAMEPAD_DPAD_DOWN; break;
        case 5: buttons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT; break;
        case 6: buttons |= XINPUT_GAMEPAD_DPAD_LEFT; break;
        case 7: buttons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT; break;
        default: break;
        }
    }

    return buttons;
}

static WORD ApplyOverlayControllerButtons(WORD rawButtons, WORD& prevButtons, ULONGLONG now, bool applyActions = true) {
    const bool comboNow = IsControllerComboPressed(rawButtons, g_cfg.controllerGuiToggleMask);
    const bool comboPrev = IsControllerComboPressed(prevButtons, g_cfg.controllerGuiToggleMask);
    const bool comboPressed = comboNow && !comboPrev;
    const bool effectComboNow = IsControllerComboPressed(rawButtons, g_cfg.controllerEffectToggleMask);
    const bool effectComboPrev = IsControllerComboPressed(prevButtons, g_cfg.controllerEffectToggleMask);
    const bool effectComboPressed = effectComboNow && !effectComboPrev;
    const bool bPressed = ((rawButtons & XINPUT_GAMEPAD_B) != 0) && ((prevButtons & XINPUT_GAMEPAD_B) == 0);

    if (applyActions && comboPressed) {
        g_menuOpen = !g_menuOpen;
        g_uiTabSelectRequested = false;
        g_uiControllerMode.store(g_menuOpen);
        g_uiInputSuppressUntil.store(now + 220);
        g_uiConsumeUntil.store(now + 260);
        ResetControllerRepeatState();
        if (g_menuOpen) {
            ResetOverlayOpenDiagnostics();
            LogOverlayOpenSnapshot("controller");
        } else {
            ClearCachedOverlayDrawData();
        }
        if (!g_menuOpen && g_uiScaleConfigDirty) {
            SaveConfigUIScale();
            g_uiScaleConfigDirty = false;
        }
    } else if (applyActions && g_menuOpen && bPressed) {
        if (g_uiPresetPopupOpen.load()) {
            g_uiRequestClosePopup.store(true);
        } else {
            g_menuOpen = false;
            g_uiTabSelectRequested = false;
            g_uiControllerMode.store(false);
            g_uiInputSuppressUntil.store(now + 120);
            g_uiConsumeUntil.store(now + 260);
            ResetControllerRepeatState();
            ClearCachedOverlayDrawData();
            if (g_uiScaleConfigDirty) {
                SaveConfigUIScale();
                g_uiScaleConfigDirty = false;
            }
        }
    }
    if (applyActions && effectComboPressed) {
        ToggleModEnabled();
        g_uiConsumeUntil.store(now + 260);
    }

    WORD consumeMask = 0;
    if (g_menuOpen || comboNow || comboPressed || bPressed || now < g_uiConsumeUntil.load()) {
        consumeMask |= XINPUT_GAMEPAD_DPAD_UP;
        consumeMask |= XINPUT_GAMEPAD_DPAD_DOWN;
        consumeMask |= XINPUT_GAMEPAD_DPAD_LEFT;
        consumeMask |= XINPUT_GAMEPAD_DPAD_RIGHT;
        consumeMask |= XINPUT_GAMEPAD_A;
        consumeMask |= XINPUT_GAMEPAD_B;
        consumeMask |= XINPUT_GAMEPAD_X;
        consumeMask |= XINPUT_GAMEPAD_Y;
        consumeMask |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        consumeMask |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    }
    if (effectComboNow || effectComboPressed) {
        consumeMask = WORD(consumeMask | g_cfg.controllerEffectToggleMask);
    }
    if (comboNow || comboPressed) {
        consumeMask = WORD(consumeMask | g_cfg.controllerGuiToggleMask);
    }

    if (applyActions) {
        prevButtons = rawButtons;
    }
    return WORD(rawButtons & ~consumeMask);
}

static void FilterDualSenseOverlayButtons(HANDLE hDevice, BYTE* report, ULONG reportLen, WORD rawButtons, WORD filteredButtons) {
    if (!hDevice || !report || reportLen == 0 || rawButtons == filteredButtons) return;

    std::vector<BYTE> preparsedStorage;
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!GetDualSensePreparsedData(hDevice, preparsedStorage, &preparsed) || !preparsed)
        return;

    USAGE usagesToClear[6] = {};
    ULONG usageCount = 0;
    auto queueUsage = [&](WORD mask, USAGE usage) {
        if ((rawButtons & mask) != 0 && (filteredButtons & mask) == 0 && usageCount < std::size(usagesToClear)) {
            usagesToClear[usageCount++] = usage;
        }
    };
    queueUsage(XINPUT_GAMEPAD_X, 1);
    queueUsage(XINPUT_GAMEPAD_A, 2);
    queueUsage(XINPUT_GAMEPAD_B, 3);
    queueUsage(XINPUT_GAMEPAD_Y, 4);
    queueUsage(XINPUT_GAMEPAD_LEFT_SHOULDER, 5);
    queueUsage(XINPUT_GAMEPAD_RIGHT_SHOULDER, 6);
    if (usageCount != 0) {
        ULONG clearCount = usageCount;
        const NTSTATUS unsetStatus =
            HidP_UnsetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, usagesToClear, &clearCount,
                preparsed, reinterpret_cast<PCHAR>(report), reportLen);
        (void)unsetStatus;
    }

    const WORD rawDpad = rawButtons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
        XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
    const WORD filteredDpad = filteredButtons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
        XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT);
    if (rawDpad != filteredDpad) {
        const NTSTATUS setStatus =
            HidP_SetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_HATSWITCH, 8,
                preparsed, reinterpret_cast<PCHAR>(report), reportLen);
        (void)setStatus;
    }
}

static void ProcessDualSenseRawInput(RAWINPUT* raw, bool filterReports, bool applyActions) {
    if (!raw || raw->header.dwType != RIM_TYPEHID || !IsDualSenseHidDevice(raw->header.hDevice))
        return;

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG lastXInputTick = g_lastXInputSeenTick.load();
    if (lastXInputTick != 0 && (now - lastXInputTick) <= 1500ULL) {
        if (!g_dualSenseSuppressedByXInputLogged) {
            g_dualSenseSuppressedByXInputLogged = true;
            Log("[input] Suppressing native DualSense raw input because XInput is active\n");
        }
        return;
    }

    const ULONG reportSize = raw->data.hid.dwSizeHid;
    const ULONG reportCount = raw->data.hid.dwCount;
    BYTE* reportBytes = raw->data.hid.bRawData;
    if (reportSize == 0 || reportCount == 0 || !reportBytes) return;

    for (ULONG i = 0; i < reportCount; ++i) {
        BYTE* report = reportBytes + (reportSize * i);
        const WORD rawButtons = MapDualSenseReportToXInputButtons(raw->header.hDevice, report, reportSize);
        g_dualSenseButtons = rawButtons;
        g_dualSenseLastInputMs = now;
        g_lastDualSenseRawPacketTick.store(now);
        const WORD filteredButtons = ApplyOverlayControllerButtons(rawButtons, g_dualSenseButtonsPrev, now, applyActions);
        if (!g_dualSenseFirstPacketLogged) {
            g_dualSenseFirstPacketLogged = true;
            LogRawInputHidDevice(raw->header.hDevice, "dualsense-first-packet");
            Log("[input] DualSense packet reportSize=%lu reportCount=%lu buttons=0x%04X filtered=0x%04X apply=%d filter=%d\n",
                reportSize, reportCount, rawButtons, filteredButtons, applyActions ? 1 : 0, filterReports ? 1 : 0);
        } else if (rawButtons != 0 && !g_dualSenseFirstActivePacketLogged) {
            g_dualSenseFirstActivePacketLogged = true;
            Log("[input] DualSense active buttons=0x%04X filtered=0x%04X apply=%d filter=%d\n",
                rawButtons, filteredButtons, applyActions ? 1 : 0, filterReports ? 1 : 0);
        }
        if (filterReports) {
            FilterDualSenseOverlayButtons(raw->header.hDevice, report, reportSize, rawButtons, filteredButtons);
        }
    }
}

static void LogActiveGpu(ID3D12Device* device) {
    (void)device;
}

static void ClearCachedOverlayDrawData() {
    for (ImDrawList* drawList : g_cachedOverlayDrawData.drawLists) {
        IM_DELETE(drawList);
    }
    g_cachedOverlayDrawData.drawLists.clear();
    g_cachedOverlayDrawData.drawData.Clear();
    g_cachedOverlayDrawData.valid = false;
}

static void RegisterDualSenseRawInput(HWND hwnd) {
    if (!hwnd || g_dualSenseRawInputHwnd == hwnd) return;
    if (!HasSupportedSonyRawInputDevice()) {
        Log("[input] Skipping native DualSense raw registration because no supported Sony HID device is present\n");
        return;
    }

    RAWINPUTDEVICE devices[2] = {};
    devices[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    devices[0].usUsage = HID_USAGE_GENERIC_GAMEPAD;
    devices[0].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    devices[0].hwndTarget = hwnd;

    devices[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    devices[1].usUsage = HID_USAGE_GENERIC_JOYSTICK;
    devices[1].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    devices[1].hwndTarget = hwnd;

    if (RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) {
        g_dualSenseRawInputHwnd = hwnd;
        Log("[+] Registered raw HID input for gamepad/joystick on hwnd=%p (DualSense native path armed)\n", hwnd);
    } else {
        Log("[W] RegisterRawInputDevices failed for DualSense path gle=%lu\n", GetLastError());
    }
}

static void CacheOverlayDrawData(const ImDrawData* src) {
    ClearCachedOverlayDrawData();
    if (!src || !src->Valid) return;

    g_cachedOverlayDrawData.drawData.Valid = true;
    g_cachedOverlayDrawData.drawData.DisplayPos = src->DisplayPos;
    g_cachedOverlayDrawData.drawData.DisplaySize = src->DisplaySize;
    g_cachedOverlayDrawData.drawData.FramebufferScale = src->FramebufferScale;
    g_cachedOverlayDrawData.drawData.OwnerViewport = src->OwnerViewport;
    g_cachedOverlayDrawData.drawData.Textures = src->Textures;

    for (ImDrawList* drawList : src->CmdLists) {
        ImDrawList* clone = drawList ? drawList->CloneOutput() : nullptr;
        if (!clone) continue;
        g_cachedOverlayDrawData.drawLists.push_back(clone);
        g_cachedOverlayDrawData.drawData.AddDrawList(clone);
    }

    g_cachedOverlayDrawData.valid = (g_cachedOverlayDrawData.drawData.CmdListsCount > 0 || src->CmdListsCount == 0);
}

static ImDrawData* PrepareOverlayDrawData(bool reuseCachedDrawData) {
    if (!g_imguiReady || !g_menuOpen) {
        ClearCachedOverlayDrawData();
        return nullptr;
    }

    if (reuseCachedDrawData && g_cachedOverlayDrawData.valid) {
        return &g_cachedOverlayDrawData.drawData;
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    UpdateControllerNavInput();
    ImGui::NewFrame();
    DrawImGuiMenu();
    SyncMenuCursorState();
    ImGui::Render();
    CacheOverlayDrawData(ImGui::GetDrawData());
    return g_cachedOverlayDrawData.valid ? &g_cachedOverlayDrawData.drawData : nullptr;
}

static void AppendTransitionBarrier(
    std::vector<D3D12_RESOURCE_BARRIER>& barriers,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES beforeState,
    D3D12_RESOURCE_STATES afterState) {
    if (!resource || beforeState == afterState) return;

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = beforeState;
    barrier.Transition.StateAfter = afterState;
    barriers.push_back(barrier);
}

static bool RenderOverlayOnCommandList(
    ID3D12GraphicsCommandList* commandList,
    ID3D12Resource* renderTarget,
    D3D12_RESOURCE_STATES renderTargetBeforeState,
    D3D12_RESOURCE_STATES renderTargetAfterState,
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
    bool reuseCachedDrawData) {
    if (!commandList || !renderTarget || !g_dx12SrvHeap) return false;

    ImDrawData* drawData = PrepareOverlayDrawData(reuseCachedDrawData);
    if (!drawData) return false;

    std::vector<D3D12_RESOURCE_BARRIER> preBarriers;
    AppendTransitionBarrier(preBarriers, renderTarget, renderTargetBeforeState, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (!preBarriers.empty()) {
        commandList->ResourceBarrier(static_cast<UINT>(preBarriers.size()), preBarriers.data());
    }

    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
    ID3D12DescriptorHeap* heaps[] = { g_dx12SrvHeap };
    commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(drawData, commandList);

    std::vector<D3D12_RESOURCE_BARRIER> postBarriers;
    AppendTransitionBarrier(postBarriers, renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, renderTargetAfterState);
    if (!postBarriers.empty()) {
        commandList->ResourceBarrier(static_cast<UINT>(postBarriers.size()), postBarriers.data());
    }

    return true;
}

static bool IsBlockedInputMessage(UINT msg) {
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) return true;
    if (msg == WM_INPUT || msg == WM_INPUT_DEVICE_CHANGE) return true;
    if (g_imguiReady && ImGui::GetCurrentContext()) {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantTextInput) {
            switch (msg) {
            case WM_KEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYDOWN:
            case WM_SYSKEYUP:
            case WM_CHAR:
            case WM_SYSCHAR:
            case WM_DEADCHAR:
            case WM_SYSDEADCHAR:
            case WM_UNICHAR:
            case WM_IME_STARTCOMPOSITION:
            case WM_IME_ENDCOMPOSITION:
            case WM_IME_COMPOSITION:
                return true;
            default:
                break;
            }
        }
    }
    return false;
}

static BOOL WINAPI Hooked_SetCursorPos(int X, int Y) {
    if (g_menuOpen) {
        (void)X; (void)Y;
        return TRUE;
    }
    return g_pOrigSetCursorPos ? g_pOrigSetCursorPos(X, Y) : FALSE;
}

static BOOL WINAPI Hooked_ClipCursor(const RECT* lpRect) {
    if (g_menuOpen) {
        return g_pOrigClipCursor ? g_pOrigClipCursor(nullptr) : TRUE;
    }
    return g_pOrigClipCursor ? g_pOrigClipCursor(lpRect) : FALSE;
}

static UINT WINAPI Hooked_GetRawInputData(
    HRAWINPUT hRawInput, UINT uiCommand, LPVOID pData, PUINT pcbSize, UINT cbSizeHeader) {
    if (!g_pOrigGetRawInputData) return static_cast<UINT>(-1);

    const UINT result = g_pOrigGetRawInputData(hRawInput, uiCommand, pData, pcbSize, cbSizeHeader);
    if (uiCommand != RID_INPUT || !pData || result == static_cast<UINT>(-1)) {
        return result;
    }
    if (result < sizeof(RAWINPUTHEADER)) return result;

    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(pData);
    if (raw->header.dwType == RIM_TYPEMOUSE) {
        if (!g_menuOpen) return result;
        raw->data.mouse.lLastX = 0;
        raw->data.mouse.lLastY = 0;
        raw->data.mouse.usButtonFlags = 0;
        raw->data.mouse.usButtonData = 0;
    }
    return result;
}

static DWORD WINAPI Hooked_XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) {
    if (!g_pOrigXInputGetState) return ERROR_DEVICE_NOT_CONNECTED;
    DWORD hr = g_pOrigXInputGetState(dwUserIndex, pState);
    if (hr != ERROR_SUCCESS || !pState || dwUserIndex != 0) return hr;

    if (!g_xinputFirstPacketLogged) {
        g_xinputFirstPacketLogged = true;
        Log("[input] XInput controller active on slot 0 buttons=0x%04X altPadSeen=%d\n",
            pState->Gamepad.wButtons,
            g_gameInputFirstDeviceLogged ? 1 : 0);
    }
    g_lastXInputSeenTick.store(GetTickCount64());

    WORD& buttons = pState->Gamepad.wButtons;
    buttons = ApplyOverlayControllerButtons(buttons, g_padButtonsPrev, GetTickCount64());
    return hr;
}

void SetupInputHooks() {
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (!user32) user32 = LoadLibraryA("user32.dll");
    if (!user32) return;
    void* pSetCursorPos = (void*)GetProcAddress(user32, "SetCursorPos");
    void* pClipCursor = (void*)GetProcAddress(user32, "ClipCursor");
    void* pGetRawInputData = (void*)GetProcAddress(user32, "GetRawInputData");
    if (pSetCursorPos) {
        InstallHook(pSetCursorPos, (void*)&Hooked_SetCursorPos,
            (void**)&g_pOrigSetCursorPos, "SetCursorPos", false);
    }
    if (pClipCursor) {
        InstallHook(pClipCursor, (void*)&Hooked_ClipCursor,
            (void**)&g_pOrigClipCursor, "ClipCursor", false);
    }
    if (pGetRawInputData) {
        InstallHook(pGetRawInputData, (void*)&Hooked_GetRawInputData,
            (void**)&g_pOrigGetRawInputData, "GetRawInputData", false);
    }

    const char* xmods[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !g_pOrigXInputGetState; ++i) {
        HMODULE xm = GetModuleHandleA(xmods[i]);
        if (!xm) xm = LoadLibraryA(xmods[i]);
        if (!xm) continue;
        void* pGetState = (void*)GetProcAddress(xm, "XInputGetState");
        if (!pGetState) continue;
        InstallHook(pGetState, (void*)&Hooked_XInputGetState,
            (void**)&g_pOrigXInputGetState, "XInputGetState", false);
    }
}

static void ImGuiSrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu) {
    if (!g_dx12SrvHeap || !outCpu || !outGpu) return;
    UINT idx = g_srvNextFree++;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_dx12SrvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_dx12SrvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(idx) * g_srvDescriptorSize;
    gpu.ptr += UINT64(idx) * UINT64(g_srvDescriptorSize);
    *outCpu = cpu;
    *outGpu = gpu;
}

static void ImGuiSrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {
}

static void ReleaseDx12RenderTargets() {
    for (UINT i = 0; i < g_frameCount; ++i) {
        if (g_dx12Frames[i].renderTarget) { g_dx12Frames[i].renderTarget->Release(); g_dx12Frames[i].renderTarget = nullptr; }
    }
}

static void ReleaseDx12Allocators() {
    for (UINT i = 0; i < g_frameCount; ++i) {
        if (g_dx12Frames[i].cmdAllocator) { g_dx12Frames[i].cmdAllocator->Release(); g_dx12Frames[i].cmdAllocator = nullptr; }
        g_dx12Frames[i].fenceValue = 0;
    }
}

static void WaitForDx12GpuIdle() {
    if (!g_dx12Fence || !g_dx12FenceEvent) return;
    ID3D12CommandQueue* queue = AcquireOverlayQueueRef();
    if (!queue) return;
    const UINT64 fv = ++g_dx12FenceLastValue;
    if (FAILED(queue->Signal(g_dx12Fence, fv))) {
        queue->Release();
        return;
    }
    if (g_dx12Fence->GetCompletedValue() >= fv) {
        queue->Release();
        return;
    }
    g_dx12Fence->SetEventOnCompletion(fv, g_dx12FenceEvent);
    WaitForSingleObject(g_dx12FenceEvent, INFINITE);
    queue->Release();
}

static void ShutdownImGuiRuntime(bool restoreWndProc) {
    ClearCachedOverlayDrawData();
    UnfreezeOverlayQueueCapture();

    if (g_imguiReady) {
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_imguiReady = false;
        g_imguiBaseStyleValid = false;
    }

    WaitForDx12GpuIdle();
    ReleaseDx12RenderTargets();
    ReleaseDx12Allocators();

    if (g_dx12CmdList) { g_dx12CmdList->Release(); g_dx12CmdList = nullptr; }
    if (g_dx12RtvHeap) { g_dx12RtvHeap->Release(); g_dx12RtvHeap = nullptr; }
    if (g_dx12SrvHeap) { g_dx12SrvHeap->Release(); g_dx12SrvHeap = nullptr; }
    if (g_dx12Fence) { g_dx12Fence->Release(); g_dx12Fence = nullptr; }
    if (g_dx12FenceEvent) { CloseHandle(g_dx12FenceEvent); g_dx12FenceEvent = nullptr; }
    if (g_dx12Device) { g_dx12Device->Release(); g_dx12Device = nullptr; }
    if (g_swapChain3) { g_swapChain3->Release(); g_swapChain3 = nullptr; }
    g_lastOverlayRenderedSwapChain = nullptr;
    g_lastOverlayRenderedBufferIndex = UINT_MAX;
    g_lastOverlayRenderTick = 0;

    g_frameCount = 0;
    g_srvNextFree = 0;
    g_imguiInitFailed = false;

    if (restoreWndProc && g_gameHwnd && g_origGameWndProc) {
        SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)g_origGameWndProc);
        g_origGameWndProc = nullptr;
    }
}

static bool ImGuiInitFailOnce(const char* reason) {
    if (!g_imguiInitFailed) {
        Log("[E] ImGui DX12 init failed: %s\n", reason ? reason : "unknown");
        g_imguiInitFailed = true;
    }
    return false;
}

enum class DxgiHookKind : uint8_t {
    FactoryCreateSwapChain,
    Factory2CreateSwapChainForHwnd,
    SwapChainPresent,
    SwapChainResizeBuffers,
};

struct DxgiHookEntry {
    void* target = nullptr;
    void* original = nullptr;
    DxgiHookKind kind = DxgiHookKind::FactoryCreateSwapChain;
    int slotIndex = -1;
    bool enabled = false;
    ULONGLONG lastSeenTick = 0;
    char moduleName[MAX_PATH] = {};
    char modulePath[MAX_PATH] = {};
};

static constexpr int kMaxDxgiHookEntries = 32;
static DxgiHookEntry g_factoryHookEntries[kMaxDxgiHookEntries] = {};
static DxgiHookEntry g_swapChainHookEntries[kMaxDxgiHookEntries] = {};
static int g_factoryHookEntryCount = 0;
static int g_swapChainHookEntryCount = 0;
static SRWLOCK g_dxgiHookRegistryLock = SRWLOCK_INIT;

static const char* DxgiHookKindName(DxgiHookKind kind) {
    switch (kind) {
    case DxgiHookKind::FactoryCreateSwapChain: return "IDXGIFactory::CreateSwapChain";
    case DxgiHookKind::Factory2CreateSwapChainForHwnd: return "IDXGIFactory2::CreateSwapChainForHwnd";
    case DxgiHookKind::SwapChainPresent: return "IDXGISwapChain::Present";
    case DxgiHookKind::SwapChainResizeBuffers: return "IDXGISwapChain::ResizeBuffers";
    default: return "UnknownDXGIHook";
    }
}

static void DescribeAddressOwner(void* addr, char* outName, size_t outNameSize, char* outPath, size_t outPathSize) {
    if (outName && outNameSize) outName[0] = '\0';
    if (outPath && outPathSize) outPath[0] = '\0';
    if (!addr) return;

    HMODULE mod = nullptr;
    if (!GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(addr), &mod) || !mod) {
        if (outName && outNameSize) strcpy_s(outName, outNameSize, "<unknown>");
        if (outPath && outPathSize) strcpy_s(outPath, outPathSize, "<unknown>");
        return;
    }

    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(mod, path, MAX_PATH) > 0) {
        const char* leaf = strrchr(path, '\\');
        if (!leaf) leaf = path;
        else ++leaf;
        if (outName && outNameSize) strncpy_s(outName, outNameSize, leaf, _TRUNCATE);
        if (outPath && outPathSize) strncpy_s(outPath, outPathSize, path, _TRUNCATE);
        return;
    }

    if (outName && outNameSize) strcpy_s(outName, outNameSize, "<unknown>");
    if (outPath && outPathSize) strcpy_s(outPath, outPathSize, "<unknown>");
}

static DxgiHookEntry* FindDxgiHookEntryUnlocked(DxgiHookEntry* entries, int count, void* target) {
    for (int i = 0; i < count; ++i) {
        if (entries[i].target == target) return &entries[i];
    }
    return nullptr;
}

static bool CreateDxgiHook(
    DxgiHookEntry* entries, int& count, void* target, void* detour, DxgiHookKind kind, int slotIndex) {
    if (!target) return false;

    AcquireSRWLockExclusive(&g_dxgiHookRegistryLock);
    DxgiHookEntry* existing = FindDxgiHookEntryUnlocked(entries, count, target);
    if (existing) {
        ReleaseSRWLockExclusive(&g_dxgiHookRegistryLock);
        return existing->original != nullptr;
    }

    if (count >= kMaxDxgiHookEntries) {
        Log("[W] DXGI registry full for %s target=%p\n", DxgiHookKindName(kind), target);
        ReleaseSRWLockExclusive(&g_dxgiHookRegistryLock);
        return false;
    }

    void* original = nullptr;
    const MH_STATUS createStatus = MH_CreateHook(target, detour, &original);
    if (createStatus != MH_OK || !original) {
        Log("[W] Failed to create %s target=%p status=%d\n",
            DxgiHookKindName(kind), target, (int)createStatus);
        ReleaseSRWLockExclusive(&g_dxgiHookRegistryLock);
        return false;
    }

    DxgiHookEntry& entry = entries[count++];
    entry.target = target;
    entry.original = original;
    entry.kind = kind;
    entry.slotIndex = slotIndex;
    entry.enabled = false;
    DescribeAddressOwner(target, entry.moduleName, sizeof(entry.moduleName), entry.modulePath, sizeof(entry.modulePath));

    if (entries == g_swapChainHookEntries) {
        g_swapChainHooksInstalled = (g_swapChainHookEntryCount > 0);
    }
    ReleaseSRWLockExclusive(&g_dxgiHookRegistryLock);
    return true;
}

static bool EnableDxgiHook(
    DxgiHookEntry* entries, int count, void* target, const char* label) {
    if (!target) return false;

    AcquireSRWLockExclusive(&g_dxgiHookRegistryLock);
    DxgiHookEntry* entry = FindDxgiHookEntryUnlocked(entries, count, target);
    if (!entry) {
        ReleaseSRWLockExclusive(&g_dxgiHookRegistryLock);
        Log("[W] Cannot enable unregistered DXGI hook %s target=%p\n", label ? label : "<unknown>", target);
        return false;
    }
    if (entry->enabled) {
        ReleaseSRWLockExclusive(&g_dxgiHookRegistryLock);
        return true;
    }
    ReleaseSRWLockExclusive(&g_dxgiHookRegistryLock);

    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK) {
        Log("[W] Failed to enable %s target=%p status=%d\n",
            label ? label : "<unknown>", target, (int)enableStatus);
        return false;
    }

    AcquireSRWLockExclusive(&g_dxgiHookRegistryLock);
    entry = FindDxgiHookEntryUnlocked(entries, count, target);
    if (entry) entry->enabled = true;
    ReleaseSRWLockExclusive(&g_dxgiHookRegistryLock);

    Log("[+] Hooked %s at %p\n", label ? label : "<unknown>", target);
    return true;
}

static void* ResolveDxgiOriginalForObject(
    void* self, int slotIndex, DxgiHookEntry* entries, int count, const char* label) {
    if (!self) return nullptr;
    void** vtbl = *reinterpret_cast<void***>(self);
    if (!vtbl) return nullptr;
    void* target = vtbl[slotIndex];

    void* original = nullptr;
    AcquireSRWLockShared(&g_dxgiHookRegistryLock);
    if (DxgiHookEntry* entry = FindDxgiHookEntryUnlocked(entries, count, target)) {
        original = entry->original;
    }
    ReleaseSRWLockShared(&g_dxgiHookRegistryLock);

    if (!original) {
        static ULONGLONG s_lastWarnTick = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_lastWarnTick > 1500ULL) {
            s_lastWarnTick = now;
            char leaf[MAX_PATH] = {};
            char path[MAX_PATH] = {};
            DescribeAddressOwner(target, leaf, sizeof(leaf), path, sizeof(path));
            Log("[W] Unresolved original for %s target=%p module=%s path=%s\n",
                label ? label : "<unknown>", target, leaf, path[0] ? path : "<unknown>");
        }
    }
    return original;
}

static void* ResolveAnyDxgiOriginal(DxgiHookEntry* entries, int count, DxgiHookKind kind) {
    void* original = nullptr;
    AcquireSRWLockShared(&g_dxgiHookRegistryLock);
    for (int i = 0; i < count; ++i) {
        if (entries[i].kind == kind && entries[i].original) {
            original = entries[i].original;
            break;
        }
    }
    ReleaseSRWLockShared(&g_dxgiHookRegistryLock);
    return original;
}

static SwapChainPresent_fn ResolveSwapChainPresentOriginal(IDXGISwapChain3* self) {
    void* original = ResolveDxgiOriginalForObject(
        self, 8, g_swapChainHookEntries, g_swapChainHookEntryCount, "IDXGISwapChain::Present");
    if (!original) {
        original = ResolveAnyDxgiOriginal(g_swapChainHookEntries, g_swapChainHookEntryCount, DxgiHookKind::SwapChainPresent);
        if (original) {
            CW_LOG_ONCE(s_presentFallbackOriginalLogged,
                "[W] Present original unresolved for current target; using first known Present trampoline\n");
        }
    }
    return reinterpret_cast<SwapChainPresent_fn>(original);
}

static SwapChainResizeBuffers_fn ResolveSwapChainResizeBuffersOriginal(IDXGISwapChain3* self) {
    void* original = ResolveDxgiOriginalForObject(
        self, 13, g_swapChainHookEntries, g_swapChainHookEntryCount, "IDXGISwapChain::ResizeBuffers");
    if (!original) {
        original = ResolveAnyDxgiOriginal(g_swapChainHookEntries, g_swapChainHookEntryCount, DxgiHookKind::SwapChainResizeBuffers);
        if (original) {
            CW_LOG_ONCE(s_resizeFallbackOriginalLogged,
                "[W] ResizeBuffers original unresolved for current target; using first known ResizeBuffers trampoline\n");
        }
    }
    return reinterpret_cast<SwapChainResizeBuffers_fn>(original);
}

static void TouchSwapChainHookSeen(IDXGISwapChain3* self, int slotIndex) {
    if (!self) return;
    void** vtbl = *reinterpret_cast<void***>(self);
    if (!vtbl) return;

    AcquireSRWLockExclusive(&g_dxgiHookRegistryLock);
    if (DxgiHookEntry* entry = FindDxgiHookEntryUnlocked(g_swapChainHookEntries, g_swapChainHookEntryCount, vtbl[slotIndex])) {
        entry->lastSeenTick = GetTickCount64();
    }
    ReleaseSRWLockExclusive(&g_dxgiHookRegistryLock);
}

static bool ShouldRenderOverlayFrame(IDXGISwapChain3* activeSwapChain, UINT frameIdx, ULONGLONG nowTick) {
    if (!activeSwapChain) return false;

    if (g_lastOverlayRenderedSwapChain == activeSwapChain &&
        g_lastOverlayRenderedBufferIndex == frameIdx &&
        nowTick - g_lastOverlayRenderTick < 250ULL) {
        return false;
    }

    g_lastOverlayRenderedSwapChain = activeSwapChain;
    g_lastOverlayRenderedBufferIndex = frameIdx;
    g_lastOverlayRenderTick = nowTick;
    return true;
}

static bool WaitForOverlayFrameFence(Dx12FrameCtx& frame, UINT frameIdx) {
    if (frame.fenceValue == 0 || !g_dx12Fence || !g_dx12FenceEvent) return true;
    if (g_dx12Fence->GetCompletedValue() >= frame.fenceValue) {
        frame.fenceValue = 0;
        return true;
    }

    g_dx12Fence->SetEventOnCompletion(frame.fenceValue, g_dx12FenceEvent);
    const DWORD waitResult = WaitForSingleObject(g_dx12FenceEvent, 100);
    if (waitResult == WAIT_OBJECT_0) {
        frame.fenceValue = 0;
        return true;
    }

    if (!g_overlayFenceTimeoutWarned) {
        g_overlayFenceTimeoutWarned = true;
        Log("[W] Overlay fence wait timed out on frame %u; skipping overlay work for this Present\n", frameIdx);
    }
    return false;
}

static bool RecordOverlayCommandList(IDXGISwapChain3* activeSwapChain, ULONGLONG nowTick, Dx12FrameCtx** outFrameCtx) {
    if (outFrameCtx) *outFrameCtx = nullptr;
    if (!activeSwapChain || !g_imguiReady || !g_menuOpen || !g_dx12CmdList) return false;
    ID3D12CommandQueue* queue = AcquireOverlayQueueRef();
    if (!queue) return false;
    queue->Release();

    UINT frameIdx = activeSwapChain->GetCurrentBackBufferIndex();
    if (frameIdx >= g_frameCount) return false;
    if (!ShouldRenderOverlayFrame(activeSwapChain, frameIdx, nowTick)) return false;

    Dx12FrameCtx& frame = g_dx12Frames[frameIdx];
    if (!WaitForOverlayFrameFence(frame, frameIdx)) return false;

    frame.cmdAllocator->Reset();
    g_dx12CmdList->Reset(frame.cmdAllocator, nullptr);

    if (!RenderOverlayOnCommandList(
            g_dx12CmdList,
            frame.renderTarget,
            D3D12_RESOURCE_STATE_PRESENT,
            D3D12_RESOURCE_STATE_PRESENT,
            frame.rtvHandle,
            false)) {
        g_dx12CmdList->Close();
        return false;
    }

    g_dx12CmdList->Close();

    if (outFrameCtx) *outFrameCtx = &frame;
    return true;
}

static void MarkOverlaySubmissionQueued(ID3D12CommandQueue* queue, Dx12FrameCtx& frame) {
    if (!queue || !g_dx12Fence) return;
    const UINT64 fv = ++g_dx12FenceLastValue;
    queue->Signal(g_dx12Fence, fv);
    frame.fenceValue = fv;
}

static void DrawImGuiMenu() {
    Preset_EnsureInitialized();
    const ImVec2 baseSize(540.0f, 320.0f);
    const ImVec2 scaledBase(baseSize.x * g_uiScaleApplied, baseSize.y * g_uiScaleApplied);
    const float resetBtnWidth = 26.0f * g_uiScaleApplied;
    const char* kUniformSliderLabels[] = {
        "Rain",
        "Dust",
        "Snow",
        "Time",
        "Cloud Height",
        "Cloud Density",
        "Mid Clouds",
        "High Clouds",
        "2C",
        "2D",
        "Night Sky Rotation X [0A]",
        "Fog",
        "Wind",
        "Puddle Scale",
        "UI Scale"
    };
    float maxSliderLabelWidth = 0.0f;
    for (const char* label : kUniformSliderLabels) {
        maxSliderLabelWidth = max(maxSliderLabelWidth, ImGui::CalcTextSize(label).x);
    }
    const ImGuiStyle& preBeginStyle = ImGui::GetStyle();
    const float sliderMinWidth = 260.0f * g_uiScaleApplied;
    const float sliderLayoutPad = 8.0f * g_uiScaleApplied;
    const float minOverlayWidth = max(
        scaledBase.x,
        (preBeginStyle.WindowPadding.x * 2.0f) +
            sliderMinWidth +
            maxSliderLabelWidth +
            resetBtnWidth +
            (preBeginStyle.ItemSpacing.x * 2.0f) +
            sliderLayoutPad);
    ImGui::SetNextWindowSize(scaledBase, ImGuiCond_FirstUseEver);
    if (g_uiScaleWindowResizePending) {
        ImGui::SetNextWindowSize(scaledBase, ImGuiCond_Always);
        g_uiScaleWindowResizePending = false;
    }
    ImGui::SetNextWindowSizeConstraints(ImVec2(minOverlayWidth, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("Crimson Weather " MOD_VERSION, &g_menuOpen, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    char guiComboText[64] = {};
    char effectComboText[64] = {};
    ControllerComboToDisplayString(g_cfg.controllerGuiToggleMask, guiComboText, sizeof(guiComboText));
    ControllerComboToDisplayString(g_cfg.controllerEffectToggleMask, effectComboText, sizeof(effectComboText));
    const std::string guiHotkeyText = VKToKeyName(g_cfg.hotkeyVK);
    const std::string effectHotkeyText = VKToKeyName(g_cfg.effectToggleVK);
    ImGui::Text("Toggle UI: %s | %s", guiHotkeyText.c_str(), guiComboText);
    ImGui::Text("Toggle Weather: %s | %s", effectHotkeyText.c_str(), effectComboText);
    ImGui::Separator();

    if (!g_modEnabled.load()) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Crimson Weather is currently disabled");
        ImGui::Spacing();

        const char* enableLabel = "Enable Crimson Weather";
        const bool enablePressed = ImGui::Button(enableLabel, ImVec2(220.0f * g_uiScaleApplied, 0.0f)) ||
                                   g_padCmd.a;
        if (enablePressed) {
            SetModEnabled(true);
            GUI_SetStatus("Weather control enabled");
        }

        ImGui::End();
        return;
    }

    static bool s_sliderEditMode = false;
    static char s_newPresetName[128] = "NewPreset.ini";
    static bool s_focusPresetName = false;
    constexpr int kPresetRowFocusCount = 3;
    static bool s_topRowActive = true;
    static int s_topRowFocus = 0; // 0=preset,1=save,2=refresh
    static int s_presetPopupFocus = 0; // 0=[New Preset], 1..N=presets
    static bool s_presetPopupIgnoreA = false;
    static int s_savePopupFocus = 0; // 0=Create,1=Cancel
    static bool s_savePopupIgnoreA = false;
    static int s_sliderLaneByTab[6] = { 0, 0, 0, 0, 0, 0 }; // 0=slider,1=reset

    const bool presetSelectPopupOpen = ImGui::IsPopupOpen("Preset Select");
    const bool savePresetPopupOpen = ImGui::IsPopupOpen("Save Preset");
    const bool popupNavLocked = presetSelectPopupOpen || savePresetPopupOpen;
    g_uiPresetPopupOpen.store(popupNavLocked);

    auto MoveFocus = [&](int tab, int itemCount) {
        if (itemCount <= kPresetRowFocusCount) {
            g_uiFocusByTab[tab] = kPresetRowFocusCount;
            return;
        }
        const int firstMainIndex = kPresetRowFocusCount;
        int& f = g_uiFocusByTab[tab];
        if (f < firstMainIndex) f = firstMainIndex;
        if (f >= itemCount) f = itemCount - 1;
        if (g_menuOpen && g_uiTabIndex == tab && !popupNavLocked) {
            if (s_topRowActive) {
                if (g_padCmd.left) s_topRowFocus = (s_topRowFocus + kPresetRowFocusCount - 1) % kPresetRowFocusCount;
                if (g_padCmd.right) s_topRowFocus = (s_topRowFocus + 1) % kPresetRowFocusCount;
                if (g_padCmd.down) {
                    s_topRowActive = false;
                    f = firstMainIndex;
                    s_sliderLaneByTab[tab] = 0;
                }
            } else {
                if (g_padCmd.up) {
                    s_sliderEditMode = false;
                    if (f == firstMainIndex) {
                        s_topRowActive = true;
                    } else {
                        --f;
                        s_sliderLaneByTab[tab] = 0;
                    }
                }
                if (g_padCmd.down) {
                    s_sliderEditMode = false;
                    ++f;
                    if (f >= itemCount) f = firstMainIndex;
                    s_sliderLaneByTab[tab] = 0;
                }
            }
        }
    };
    auto Focused = [&](int tab, int idx) -> bool {
        return g_menuOpen && g_uiControllerMode.load() &&
               !s_topRowActive &&
               g_uiTabIndex == tab && g_uiFocusByTab[tab] == idx;
    };
    auto FocusBegin = [&](bool focused, bool isEditing = false) {
        if (!focused) return;
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 2.0f);
        if (isEditing) {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.2f, 1.0f, 0.2f, 0.35f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.2f, 1.0f, 0.2f, 0.45f));
        }
        else {
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(1.0f, 0.85f, 0.2f, 0.35f));
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1.0f, 0.85f, 0.2f, 0.45f));
        }
        };
    auto FocusEnd = [&](bool focused) {
        if (!focused) return;
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(1);
    };
    const float sliderLabelColumnWidth = maxSliderLabelWidth + ImGui::GetStyle().ItemInnerSpacing.x;
    struct UniformSliderRowLayout {
        float rowStartX = 0.0f;
        float sliderWidth = 0.0f;
        float resetColumnX = 0.0f;
        const char* visibleLabel = "";
    };
    auto BeginUniformSliderRow = [&](const char* rowId, const char* visibleLabel) -> UniformSliderRowLayout {
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float safetyPad = 8.0f * g_uiScaleApplied;
        const float rowStartX = ImGui::GetCursorPosX();
        const float sliderWidth = max(80.0f * g_uiScaleApplied,
            ImGui::GetContentRegionAvail().x - resetBtnWidth - (spacing * 2.0f) - sliderLabelColumnWidth - safetyPad);
        const float resetColumnX = rowStartX + sliderWidth + spacing + sliderLabelColumnWidth + spacing;
        ImGui::PushID(rowId);
        ImGui::SetNextItemWidth(sliderWidth);
        return { rowStartX, sliderWidth, resetColumnX, visibleLabel };
    };

    auto DrawUniformSliderLabel = [&](const UniformSliderRowLayout& row) {
        ImGui::SameLine();
        ImGui::TextUnformatted(row.visibleLabel);
    };
    auto DrawUniformResetButton = [&](const UniformSliderRowLayout& row) -> bool {
        ImGui::SameLine(row.resetColumnX);
        const bool pressed = ImGui::Button("\xE2\xAD\xAE##reset", ImVec2(resetBtnWidth, 0.0f));
        ImGui::PopID();
        return pressed;
    };
    struct SliderLaneState {
        bool resetFocused = false;
        bool transitioned = false;
    };
    auto SliderLane = [&](int tab, int idx) -> SliderLaneState {
        SliderLaneState state{};
        if (!Focused(tab, idx) || popupNavLocked) return state;
        int& lane = s_sliderLaneByTab[tab];
        if (lane < 0 || lane > 1) lane = 0;

        if (!s_sliderEditMode) {
            if (lane == 0 && g_padCmd.right) {
                lane = 1;
                state.transitioned = true;
            }
            else if (lane == 1 && g_padCmd.left) {
                lane = 0;
                state.transitioned = true;
            }
        }
        state.resetFocused = (lane == 1);
        return state;
        };
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset:");
    ImGui::SameLine();
    const float actionButtonWidth = 70.0f * g_uiScaleApplied;
    const float comboWidth = max(150.0f * g_uiScaleApplied,
        ImGui::GetContentRegionAvail().x - (actionButtonWidth * 2.0f) - (ImGui::GetStyle().ItemSpacing.x * 2.0f));
    const bool topRowFocused = g_menuOpen && g_uiControllerMode.load() && s_topRowActive;
    const bool presetSelectorFocused = topRowFocused && (s_topRowFocus == 0);
    const bool saveButtonFocused = topRowFocused && (s_topRowFocus == 1);
    const bool refreshButtonFocused = topRowFocused && (s_topRowFocus == 2);
    if (s_topRowFocus < 0) s_topRowFocus = 0;
    if (s_topRowFocus >= kPresetRowFocusCount) s_topRowFocus = kPresetRowFocusCount - 1;

    auto ResetPresetPopupFocus = [&]() {
        s_presetPopupFocus = Preset_HasSelection() ? (Preset_GetSelectedIndex() + 1) : 0;
        if (s_presetPopupFocus < 0) s_presetPopupFocus = 0;
    };

    if (s_presetPopupIgnoreA && !g_padCmd.a) s_presetPopupIgnoreA = false;
    if (s_savePopupIgnoreA && !g_padCmd.a) s_savePopupIgnoreA = false;

    bool openPresetPopup = false;
    if (presetSelectorFocused && !popupNavLocked && g_padCmd.a) {
        openPresetPopup = true;
    }

    char presetButtonLabel[192] = {};
    sprintf_s(presetButtonLabel, "%s  v", Preset_GetSelectedDisplayName());
    FocusBegin(presetSelectorFocused);
    const bool presetClicked = ImGui::Button(presetButtonLabel, ImVec2(comboWidth, 0.0f));
    const ImVec2 presetComboMin = ImGui::GetItemRectMin();
    const ImVec2 presetComboMax = ImGui::GetItemRectMax();
    const int presetCount = Preset_GetCount();
    if (presetClicked || openPresetPopup) {
        ResetPresetPopupFocus();
        ImGui::OpenPopup("Preset Select");
        if (openPresetPopup) s_presetPopupIgnoreA = true;
    }

    if (g_uiControllerMode.load()) {
        ImGui::SetNextWindowPos(
            ImVec2(presetComboMin.x, presetComboMax.y + ImGui::GetStyle().ItemSpacing.y),
            ImGuiCond_Always);
    }

    if (ImGui::BeginPopup("Preset Select")) {
        if (g_uiRequestClosePopup.exchange(false)) {
            ImGui::CloseCurrentPopup();
        }
        if (s_presetPopupFocus < 0) s_presetPopupFocus = 0;
        if (s_presetPopupFocus > presetCount) s_presetPopupFocus = presetCount;
        if (g_uiControllerMode.load()) {
            if (g_padCmd.up) s_presetPopupFocus = (s_presetPopupFocus + presetCount + 1 - 1) % (presetCount + 1);
            if (g_padCmd.down) s_presetPopupFocus = (s_presetPopupFocus + 1) % (presetCount + 1);
        }

        bool popupHandled = false;
        const bool popupAPressed = g_padCmd.a && !s_presetPopupIgnoreA;

        const bool newPresetSelected = !Preset_HasSelection();
        const bool newPresetFocused = g_uiControllerMode.load() && s_presetPopupFocus == 0;
        if (newPresetSelected && !newPresetFocused) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.26f, 0.59f, 0.98f, 0.28f));
        }
        FocusBegin(newPresetFocused);
        const bool newPresetClicked = ImGui::Selectable("[New Preset]", newPresetSelected || newPresetFocused);
        FocusEnd(newPresetFocused);
        if (newPresetSelected && !newPresetFocused) {
            ImGui::PopStyleColor(1);
        }
        if (!popupHandled && (newPresetClicked || (popupAPressed && s_presetPopupFocus == 0))) {
            Preset_SelectNew();
            ImGui::CloseCurrentPopup();
            popupHandled = true;
        }
        if (presetCount == 0) ImGui::TextDisabled("No valid presets found");
        for (int i = 0; i < presetCount; ++i) {
            const int visualIdx = i + 1;
            const bool isSelected = (i == Preset_GetSelectedIndex());
            const bool itemFocused = g_uiControllerMode.load() && s_presetPopupFocus == visualIdx;
            if (isSelected && !itemFocused) {
                ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.26f, 0.59f, 0.98f, 0.28f));
            }
            FocusBegin(itemFocused);
            const bool presetSelected = ImGui::Selectable(Preset_GetDisplayName(i), isSelected || itemFocused);
            FocusEnd(itemFocused);
            if (isSelected && !itemFocused) {
                ImGui::PopStyleColor(1);
            }
            if (!popupHandled && (presetSelected || (popupAPressed && s_presetPopupFocus == visualIdx))) {
                Preset_SelectIndex(i);
                ImGui::CloseCurrentPopup();
                popupHandled = true;
            }
            if (isSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndPopup();
    }
    FocusEnd(presetSelectorFocused);

    const bool canSaveCurrent = Preset_CanSaveCurrent();
    ImGui::SameLine();
    if (!canSaveCurrent) ImGui::BeginDisabled();
    FocusBegin(saveButtonFocused);
    const bool savePressed = (ImGui::Button("Save", ImVec2(actionButtonWidth, 0.0f)) ||
        (saveButtonFocused && !popupNavLocked && g_padCmd.a)) && canSaveCurrent;
    FocusEnd(saveButtonFocused);
    if (!canSaveCurrent) ImGui::EndDisabled();
    if (savePressed) {
        if (Preset_HasSelection()) {
            Preset_SaveSelected();
        } else {
            strcpy_s(s_newPresetName, "NewPreset.ini");
            s_focusPresetName = true;
            ImGui::OpenPopup("Save Preset");
            s_savePopupFocus = 0;
            s_savePopupIgnoreA = true;
        }
    }

    ImGui::SameLine();
    FocusBegin(refreshButtonFocused);
    const bool refreshPressed = ImGui::Button("Refresh", ImVec2(actionButtonWidth, 0.0f)) ||
        (refreshButtonFocused && !popupNavLocked && g_padCmd.a);
    FocusEnd(refreshButtonFocused);
    if (refreshPressed) {
        Preset_Refresh();
        GUI_SetStatus("Preset list refreshed");
    }

    ImGui::SetNextWindowPos(
        ImVec2(presetComboMax.x + ImGui::GetStyle().ItemSpacing.x, presetComboMin.y),
        ImGuiCond_Appearing);
    if (ImGui::BeginPopup("Save Preset", ImGuiWindowFlags_AlwaysAutoResize)) {
        if (g_uiRequestClosePopup.exchange(false)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::TextUnformatted("Create a new preset file");
        if (ImGui::IsWindowAppearing()) {
            s_savePopupFocus = 0;
            s_savePopupIgnoreA = true;
        }
        if (g_uiControllerMode.load()) {
            if (g_padCmd.left || g_padCmd.right || g_padCmd.up || g_padCmd.down) {
                s_savePopupFocus = 1 - s_savePopupFocus;
            }
        }
        const bool popupAPressed = g_padCmd.a && !s_savePopupIgnoreA;
        if (s_focusPresetName) {
            ImGui::SetKeyboardFocusHere();
            s_focusPresetName = false;
        }
        ImGui::SetNextItemWidth(280.0f * g_uiScaleApplied);
        const bool submitted = ImGui::InputText("File name", s_newPresetName, IM_ARRAYSIZE(s_newPresetName),
            ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue);
        FocusBegin(g_uiControllerMode.load() && s_savePopupFocus == 0);
        const bool createPressed = ImGui::Button("Create") || (g_uiControllerMode.load() && s_savePopupFocus == 0 && popupAPressed);
        FocusEnd(g_uiControllerMode.load() && s_savePopupFocus == 0);
        if (submitted || createPressed) {
            if (Preset_SaveAs(s_newPresetName)) {
                strcpy_s(s_newPresetName, "NewPreset.ini");
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        FocusBegin(g_uiControllerMode.load() && s_savePopupFocus == 1);
        const bool cancelPressed = ImGui::Button("Cancel") || (g_uiControllerMode.load() && s_savePopupFocus == 1 && popupAPressed);
        FocusEnd(g_uiControllerMode.load() && s_savePopupFocus == 1);
        if (cancelPressed) {
            strcpy_s(s_newPresetName, "NewPreset.ini");
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::Separator();

    if (ImGui::BeginTabBar("cw_tabs")) {
        const int requestedTab = g_uiTabIndex;
        int activeTabSeen = g_uiTabIndex;
        auto TabFlagsFor = [&](int tabIndex) -> ImGuiTabItemFlags {
            return (g_uiTabSelectRequested && requestedTab == tabIndex) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        };

        if (ImGui::BeginTabItem("Weather", nullptr, TabFlagsFor(0))) {
            activeTabSeen = 0;
            const bool forceClearAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::ForceClear);
            const bool rainFeatureAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::Rain);
            const bool dustFeatureAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::Dust);
            const bool snowFeatureAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::Snow);
            const bool timeFeatureAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::TimeControls);
            const bool timeReady = g_timeLayoutReady.load() && timeFeatureAvailable;
            bool visualTimeOverride = g_timeCtrlActive.load() && g_timeFreeze.load();
            const int weatherResetIdx = kPresetRowFocusCount + 4 + (timeReady ? 1 : 0) + ((timeReady && visualTimeOverride) ? 1 : 0);
            int weatherItemCount = weatherResetIdx + 1;
            MoveFocus(0, weatherItemCount);

            bool forceClear = g_forceClear.load();
            if (!forceClearAvailable) ImGui::BeginDisabled();
            FocusBegin(forceClearAvailable && Focused(0, 3));
            bool forceClearChanged = ImGui::Checkbox("Force Clear Sky", &forceClear);
            FocusEnd(forceClearAvailable && Focused(0, 3));
            if (forceClearAvailable && !forceClearChanged && Focused(0, 3) && g_padCmd.a) {
                forceClear = !forceClear;
                forceClearChanged = true;
            }
            if (forceClearAvailable && forceClearChanged) {
                g_forceClear.store(forceClear);
                if (forceClear) {
                    GUI_SetStatus("Force clear active");
                } else {
                    GUI_SetStatus("Force clear off");
                }
            }
            if (!forceClearAvailable) ImGui::EndDisabled();
            ImGui::Separator();

            ImGui::TextDisabled("Rain");
            ImGui::Separator();

            float rain = g_oRain.active.load() ? g_oRain.value.load() : 0.0f;
            float snow = g_oSnow.active.load() ? g_oSnow.value.load() : 0.0f;
            float dust = g_oDust.active.load() ? g_oDust.value.load() : 0.0f;

            bool precipSlidersDisabled = forceClear || !rainFeatureAvailable;
            if (precipSlidersDisabled) ImGui::BeginDisabled();
            const SliderLaneState rainLane = SliderLane(0, 4);
            const bool rainResetFocused = rainLane.resetFocused;
            const bool rainSliderFocused = Focused(0, 4) && !rainResetFocused;

            if (!precipSlidersDisabled && rainSliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

            bool rainPadChanged = false;
            if (!precipSlidersDisabled && rainSliderFocused && s_sliderEditMode) {
                if (g_padCmd.left) { rain = max(0.0f, rain - 0.02f * g_padCmd.leftScale); rainPadChanged = true; }
                if (g_padCmd.right) { rain = min(1.0f, rain + 0.02f * g_padCmd.rightScale); rainPadChanged = true; }
            }
            const auto rainRow = BeginUniformSliderRow("rain_row", "Rain");
            FocusBegin(rainSliderFocused, rainSliderFocused && s_sliderEditMode);
            const bool rainChanged = ImGui::SliderFloat("##slider", &rain, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_NoInput) || rainPadChanged;
            FocusEnd(rainSliderFocused);
            DrawUniformSliderLabel(rainRow);
            FocusBegin(rainResetFocused);
            const bool rainReset = DrawUniformResetButton(rainRow) || (rainResetFocused && g_padCmd.a);
            FocusEnd(rainResetFocused);
            if (rainReset) {
                g_oRain.clear();
            } else if (rainChanged) {
                if (rain > 0.0001f) {
                    g_oRain.set(rain);
                } else {
                    g_oRain.clear();
                }
            }

            if (precipSlidersDisabled) ImGui::EndDisabled();

            ImGui::TextDisabled("Weather");
            ImGui::Separator();

            const SliderLaneState dustLane = SliderLane(0, 5);
            const bool dustResetFocused = dustLane.resetFocused;
            const bool dustSliderFocused = Focused(0, 5) && !dustResetFocused;

            if (dustSliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

            const bool dustControlDisabled = forceClear || !dustFeatureAvailable;
            bool dustPadChanged = false;
            if (!dustControlDisabled && dustSliderFocused && s_sliderEditMode) {
                if (g_padCmd.left) { dust = max(0.0f, dust - 0.04f * g_padCmd.leftScale); dustPadChanged = true; }
                if (g_padCmd.right) { dust = min(2.0f, dust + 0.04f * g_padCmd.rightScale); dustPadChanged = true; }
            }
            if (dustControlDisabled) ImGui::BeginDisabled();
            const auto dustRow = BeginUniformSliderRow("dust_row", "Dust");
            FocusBegin(!dustControlDisabled && dustSliderFocused, dustSliderFocused && s_sliderEditMode);
            const bool dustChanged = ImGui::SliderFloat("##slider", &dust, 0.0f, 2.0f, "%.3f", ImGuiSliderFlags_NoInput) || dustPadChanged;
            FocusEnd(!dustControlDisabled && dustSliderFocused);
            DrawUniformSliderLabel(dustRow);
            FocusBegin(!dustControlDisabled && dustResetFocused);
            const bool dustReset = DrawUniformResetButton(dustRow) || (!dustControlDisabled && dustResetFocused && g_padCmd.a);
            FocusEnd(!dustControlDisabled && dustResetFocused);
            if (!dustControlDisabled && dustReset) {
                g_oDust.clear();
            } else if (!dustControlDisabled && dustChanged) {
                (dust > 0.0001f) ? g_oDust.set(dust) : g_oDust.clear();
            }
            if (dustControlDisabled) ImGui::EndDisabled();

            const SliderLaneState snowLane = SliderLane(0, 6);
            const bool snowResetFocused = snowLane.resetFocused;
            const bool snowSliderFocused = Focused(0, 6) && !snowResetFocused;

            const bool snowControlDisabled = forceClear || !snowFeatureAvailable;
            if (!snowControlDisabled && snowSliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

            bool snowPadChanged = false;
            if (!snowControlDisabled && snowSliderFocused && s_sliderEditMode) {
                if (g_padCmd.left) { snow = max(0.0f, snow - 0.02f * g_padCmd.leftScale); snowPadChanged = true; }
                if (g_padCmd.right) { snow = min(1.0f, snow + 0.02f * g_padCmd.rightScale); snowPadChanged = true; }
            }
            if (snowControlDisabled) ImGui::BeginDisabled();
            const auto snowRow = BeginUniformSliderRow("snow_row", "Snow");
            FocusBegin(!snowControlDisabled && snowSliderFocused, snowSliderFocused && s_sliderEditMode);
            const bool snowChanged = ImGui::SliderFloat("##slider", &snow, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_NoInput) || snowPadChanged;
            FocusEnd(!snowControlDisabled && snowSliderFocused);
            DrawUniformSliderLabel(snowRow);
            FocusBegin(!snowControlDisabled && snowResetFocused);
            const bool snowReset = DrawUniformResetButton(snowRow) || (!snowControlDisabled && snowResetFocused && g_padCmd.a);
            FocusEnd(!snowControlDisabled && snowResetFocused);
            if (!snowControlDisabled && snowReset) {
                g_oSnow.clear();
            } else if (!snowControlDisabled && snowChanged) {
                (snow > 0.0001f) ? g_oSnow.set(snow) : g_oSnow.clear();
            }
            if (snowControlDisabled) ImGui::EndDisabled();

            ImGui::Separator();
            if (!timeReady) ImGui::BeginDisabled();
            FocusBegin(timeReady && Focused(0, 7));
            bool visualChanged = ImGui::Checkbox("Visual Time Override (Only Affect Visual)", &visualTimeOverride);
            FocusEnd(timeReady && Focused(0, 7));
            if (!visualChanged && timeReady && Focused(0, 7) && g_padCmd.a) {
                visualTimeOverride = !visualTimeOverride;
                visualChanged = true;
            }
            if (visualChanged) {
                if (visualTimeOverride) {
                    g_timeCtrlActive.store(true);
                    g_timeFreeze.store(true);
                    g_timeApplyRequest.store(true);
                    GUI_SetStatus("Visual time override enabled");
                } else {
                    g_timeFreeze.store(false);
                    g_timeCtrlActive.store(false);
                    g_timeApplyRequest.store(true);
                    GUI_SetStatus("Visual time override disabled");
                }
            }

            if (!timeReady || !visualTimeOverride) ImGui::BeginDisabled();
            float todHour = g_timeTargetHour.load();
            const SliderLaneState timeLane = SliderLane(0, 8);
            const bool timeResetFocused = timeLane.resetFocused;
            const bool timeSliderFocused = timeReady && visualTimeOverride && Focused(0, 8) && !timeResetFocused;

            if (timeSliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

            bool timePadChanged = false;
            if (timeSliderFocused && s_sliderEditMode) {
                if (g_padCmd.left) { todHour = max(0.0f, todHour - 0.10f * g_padCmd.leftScale); timePadChanged = true; }
                if (g_padCmd.right) { todHour = min(24.0f, todHour + 0.10f * g_padCmd.rightScale); timePadChanged = true; }
            }
            const auto timeRow = BeginUniformSliderRow("time_row", "Time");
            FocusBegin(timeSliderFocused, timeSliderFocused && s_sliderEditMode);
            const bool timeChanged = ImGui::SliderFloat("##slider", &todHour, 0.0f, 24.0f, "%.2f h", ImGuiSliderFlags_NoInput) || timePadChanged;
            FocusEnd(timeSliderFocused);
            DrawUniformSliderLabel(timeRow);
            FocusBegin(timeReady && visualTimeOverride && timeResetFocused);
            const bool timeReset = DrawUniformResetButton(timeRow) || (timeReady && visualTimeOverride && timeResetFocused && g_padCmd.a);
            FocusEnd(timeReady && visualTimeOverride && timeResetFocused);
            if (timeReset) {
                g_timeTargetHour.store(NormalizeHour24(g_timeCurrentHour.load()));
                g_timeCtrlActive.store(true);
                g_timeFreeze.store(true);
                g_timeApplyRequest.store(true);
            } else if (timeChanged) {
                g_timeTargetHour.store(NormalizeHour24(todHour));
                g_timeCtrlActive.store(true);
                g_timeFreeze.store(true);
                g_timeApplyRequest.store(true);
            }
            if (!timeReady || !visualTimeOverride) ImGui::EndDisabled();
            if (!timeReady) ImGui::EndDisabled();

            FocusBegin(Focused(0, weatherResetIdx));
            if (ImGui::Button("Reset All") || (Focused(0, weatherResetIdx) && g_padCmd.a)) {
                ResetAllSliders();
                g_activeWeather = -1;
                GUI_SetStatus("Sliders reset");
            }
            FocusEnd(Focused(0, weatherResetIdx));
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Cloud", nullptr, TabFlagsFor(1))) {
            activeTabSeen = 1;
            MoveFocus(1, kPresetRowFocusCount + 4);
            const bool forceClear = g_forceClear.load();
            const bool cloudFeatureAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::CloudControls);
            if (forceClear || !cloudFeatureAvailable) ImGui::BeginDisabled();

            float cloudSpdX = g_oCloudSpdX.active.load() ? g_oCloudSpdX.value.load() : 1.0f;
            const SliderLaneState cloudXLane = SliderLane(1, 3);
            const bool cloudXResetFocused = cloudXLane.resetFocused;
            const bool cloudXSliderFocused = Focused(1, 3) && !cloudXResetFocused;

            if (!forceClear && cloudFeatureAvailable && cloudXSliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

            bool cloudXPadChanged = false;
            if (!forceClear && cloudXSliderFocused && s_sliderEditMode) {
                if (g_padCmd.left) { cloudSpdX = max(-20.0f, cloudSpdX - 0.10f * g_padCmd.leftScale); cloudXPadChanged = true; }
                if (g_padCmd.right) { cloudSpdX = min(20.0f, cloudSpdX + 0.10f * g_padCmd.rightScale); cloudXPadChanged = true; }
            }
            const auto cloudHeightRow = BeginUniformSliderRow("cloud_height_row", "Cloud Height");
            FocusBegin(cloudXSliderFocused, cloudXSliderFocused && s_sliderEditMode);
            const bool cloudXChanged = ImGui::SliderFloat("##slider", &cloudSpdX, -20.0f, 20.0f, "%.2f", ImGuiSliderFlags_NoInput) || cloudXPadChanged;
            FocusEnd(cloudXSliderFocused);
            DrawUniformSliderLabel(cloudHeightRow);
            FocusBegin(cloudXResetFocused);
            const bool cloudXReset = DrawUniformResetButton(cloudHeightRow) || (!forceClear && cloudXResetFocused && g_padCmd.a);
            FocusEnd(cloudXResetFocused);
            if (!forceClear && cloudXReset) {
                g_oCloudSpdX.clear();
            } else if (!forceClear && cloudXChanged) {
                cloudSpdX = min(20.0f, max(-20.0f, cloudSpdX));
                g_oCloudSpdX.set(cloudSpdX);
            }
            float cloudSpdZ = g_oCloudSpdY.active.load() ? g_oCloudSpdY.value.load() : 1.0f;
            const SliderLaneState cloudZLane = SliderLane(1, 4);
            const bool cloudZResetFocused = cloudZLane.resetFocused;
            const bool cloudZSliderFocused = Focused(1, 4) && !cloudZResetFocused;

            if (!forceClear && cloudFeatureAvailable && cloudZSliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

            bool cloudZPadChanged = false;
            if (!forceClear && cloudZSliderFocused && s_sliderEditMode) {
                if (g_padCmd.left) { cloudSpdZ = max(0.0f, cloudSpdZ - 0.10f * g_padCmd.leftScale); cloudZPadChanged = true; }
                if (g_padCmd.right) { cloudSpdZ = min(10.0f, cloudSpdZ + 0.10f * g_padCmd.rightScale); cloudZPadChanged = true; }
            }
            const auto cloudDensityRow = BeginUniformSliderRow("cloud_density_row", "Cloud Density");
            FocusBegin(cloudZSliderFocused, cloudZSliderFocused && s_sliderEditMode);
            const bool cloudZChanged = ImGui::SliderFloat("##slider", &cloudSpdZ, 0.0f, 10.0f, "x%.2f", ImGuiSliderFlags_NoInput) || cloudZPadChanged;
            FocusEnd(cloudZSliderFocused);
            DrawUniformSliderLabel(cloudDensityRow);
            FocusBegin(cloudZResetFocused);
            const bool cloudZReset = DrawUniformResetButton(cloudDensityRow) || (!forceClear && cloudZResetFocused && g_padCmd.a);
            FocusEnd(cloudZResetFocused);
            if (!forceClear && cloudZReset) {
                g_oCloudSpdY.clear();
            } else if (!forceClear && cloudZChanged) {
                cloudSpdZ = min(10.0f, max(0.0f, cloudSpdZ));
                g_oCloudSpdY.set(cloudSpdZ);
            }

            auto DrawCloudSliderRow = [&](int idx, const char* rowId, const char* label,
                                          SliderOverride& slider) {
                float value = slider.active.load() ? slider.value.load() : 1.0f;
                const SliderLaneState lane = SliderLane(1, idx);
                const bool resetFocused = lane.resetFocused;
                const bool sliderFocused = Focused(1, idx) && !resetFocused;

                if (!forceClear && cloudFeatureAvailable && sliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

                bool padChanged = false;
                if (!forceClear && sliderFocused && s_sliderEditMode) {
                    if (g_padCmd.left) { value = max(0.0f, value - 0.10f * g_padCmd.leftScale); padChanged = true; }
                    if (g_padCmd.right) { value = min(15.0f, value + 0.10f * g_padCmd.rightScale); padChanged = true; }
                }

                const auto row = BeginUniformSliderRow(rowId, label);
                FocusBegin(sliderFocused, sliderFocused && s_sliderEditMode);
                const bool changed = ImGui::SliderFloat("##slider", &value, 0.0f, 15.0f, "x%.2f", ImGuiSliderFlags_NoInput) || padChanged;
                FocusEnd(sliderFocused);
                DrawUniformSliderLabel(row);
                FocusBegin(resetFocused);
                const bool reset = DrawUniformResetButton(row) || (!forceClear && resetFocused && g_padCmd.a);
                FocusEnd(resetFocused);
                if (!forceClear && reset) {
                    slider.clear();
                } else if (!forceClear && changed) {
                    value = min(15.0f, max(0.0f, value));
                    if (fabsf(value - 1.0f) <= 0.001f) slider.clear();
                    else slider.set(value);
                }
            };

            auto DrawCloudNumericRow = [&](int idx, const char* rowId, const char* label,
                                          SliderOverride& slider, float minValue, float maxValue,
                                          float step, const char* displayFormat) {
                float value = slider.active.load() ? slider.value.load() : 1.0f;
                const SliderLaneState lane = SliderLane(1, idx);
                const bool resetFocused = lane.resetFocused;
                const bool sliderFocused = Focused(1, idx) && !resetFocused;

                if (!forceClear && sliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

                bool padChanged = false;
                if (!forceClear && sliderFocused && s_sliderEditMode) {
                    if (g_padCmd.left) { value = max(minValue, value - step * g_padCmd.leftScale); padChanged = true; }
                    if (g_padCmd.right) { value = min(maxValue, value + step * g_padCmd.rightScale); padChanged = true; }
                }

                const auto row = BeginUniformSliderRow(rowId, label);
                FocusBegin(sliderFocused, sliderFocused && s_sliderEditMode);
                const bool changed = ImGui::SliderFloat("##slider", &value, minValue, maxValue, displayFormat, ImGuiSliderFlags_NoInput) || padChanged;
                FocusEnd(sliderFocused);
                DrawUniformSliderLabel(row);
                FocusBegin(resetFocused);
                const bool reset = DrawUniformResetButton(row) || (!forceClear && resetFocused && g_padCmd.a);
                FocusEnd(resetFocused);
                if (!forceClear && reset) {
                    slider.clear();
                } else if (!forceClear && changed) {
                    value = min(maxValue, max(minValue, value));
                    if (fabsf(value - 1.0f) <= 0.001f) slider.clear();
                    else slider.set(value);
                }
            };

            DrawCloudSliderRow(5, "mid_clouds_row", "Mid Clouds", g_oHighClouds);
            DrawCloudSliderRow(6, "high_clouds_row", "High Clouds", g_oAtmoAlpha);
            if (forceClear || !cloudFeatureAvailable) ImGui::EndDisabled();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Atmosphere", nullptr, TabFlagsFor(2))) {
            activeTabSeen = 2;
            MoveFocus(2, kPresetRowFocusCount + 3);
            const bool fogFeatureAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::FogControls);
            const bool windFeatureAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::WindControls);
            const bool noWindFeatureAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::NoWindControls);
            float fogPct = 0.0f;
            if (g_oFog.active.load()) {
                float fogN = sqrtf(min(1.0f, max(0.0f, g_oFog.value.load() / 100.0f)));
                fogPct = fogN * 100.0f;
            }
            const SliderLaneState fogLane = SliderLane(2, 3);
            const bool fogResetFocused = fogLane.resetFocused;
            const bool fogSliderFocused = Focused(2, 3) && !fogResetFocused;

            if (fogFeatureAvailable && fogSliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

            bool fogPadChanged = false;
            if (fogFeatureAvailable && fogSliderFocused && s_sliderEditMode) {
                if (g_padCmd.left) { fogPct = max(0.0f, fogPct - 1.5f * g_padCmd.leftScale); fogPadChanged = true; }
                if (g_padCmd.right) { fogPct = min(100.0f, fogPct + 1.5f * g_padCmd.rightScale); fogPadChanged = true; }
            }
            if (!fogFeatureAvailable) ImGui::BeginDisabled();
            const auto fogRow = BeginUniformSliderRow("fog_row", "Fog");
            FocusBegin(fogFeatureAvailable && fogSliderFocused, fogSliderFocused && s_sliderEditMode);
            const bool fogChanged = ImGui::SliderFloat("##slider", &fogPct, 0.0f, 100.0f, "%.1f%%", ImGuiSliderFlags_NoInput) || fogPadChanged;
            FocusEnd(fogFeatureAvailable && fogSliderFocused);
            DrawUniformSliderLabel(fogRow);
            FocusBegin(fogFeatureAvailable && fogResetFocused);
            const bool fogReset = DrawUniformResetButton(fogRow) || (fogFeatureAvailable && fogResetFocused && g_padCmd.a);
            FocusEnd(fogFeatureAvailable && fogResetFocused);
            if (fogFeatureAvailable && fogReset) {
                g_oFog.clear();
            } else if (fogFeatureAvailable && fogChanged) {
                float t = fogPct * 0.01f;
                float fogBoost = t * t * 100.0f;
                g_oFog.set(fogBoost);
            }
            if (!fogFeatureAvailable) ImGui::EndDisabled();

            float windMul = g_windMul.load();
            const SliderLaneState windLane = SliderLane(2, 4);
            const bool windResetFocused = windLane.resetFocused;
            const bool windSliderFocused = Focused(2, 4) && !windResetFocused;

            if (windFeatureAvailable && windSliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

            bool windPadChanged = false;
            if (windFeatureAvailable && windSliderFocused && s_sliderEditMode) {
                if (g_padCmd.left) { windMul = max(0.0f, windMul - 0.05f * g_padCmd.leftScale); windPadChanged = true; }
                if (g_padCmd.right) { windMul = min(15.0f, windMul + 0.05f * g_padCmd.rightScale); windPadChanged = true; }
            }
            if (!windFeatureAvailable) ImGui::BeginDisabled();
            const auto windRow = BeginUniformSliderRow("wind_row", "Wind");
            FocusBegin(windFeatureAvailable && windSliderFocused, windSliderFocused && s_sliderEditMode);
            const bool windChanged = ImGui::SliderFloat("##slider", &windMul, 0.0f, 15.0f, "x%.2f", ImGuiSliderFlags_NoInput) || windPadChanged;
            FocusEnd(windFeatureAvailable && windSliderFocused);
            DrawUniformSliderLabel(windRow);
            FocusBegin(windFeatureAvailable && windResetFocused);
            const bool windReset = DrawUniformResetButton(windRow) || (windFeatureAvailable && windResetFocused && g_padCmd.a);
            FocusEnd(windFeatureAvailable && windResetFocused);
            if (windFeatureAvailable && windReset) {
                g_windMul.store(1.0f);
            } else if (windFeatureAvailable && windChanged) {
                g_windMul.store(windMul);
            }
            if (!windFeatureAvailable) ImGui::EndDisabled();
            bool noWind = g_noWind.load();
            if (!noWindFeatureAvailable) ImGui::BeginDisabled();
            FocusBegin(noWindFeatureAvailable && Focused(2, 5));
            bool noWindChanged = ImGui::Checkbox("No Wind", &noWind);
            FocusEnd(noWindFeatureAvailable && Focused(2, 5));
            if (noWindFeatureAvailable && !noWindChanged && Focused(2, 5) && g_padCmd.a) {
                noWind = !noWind;
                noWindChanged = true;
            }
            if (noWindFeatureAvailable && noWindChanged) {
                g_noWind.store(noWind);
                GUI_SetStatus(noWind ? "No Wind enabled" : "No Wind disabled");
            }
            if (!noWindFeatureAvailable) ImGui::EndDisabled();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Detail", nullptr, TabFlagsFor(3))) {
            activeTabSeen = 3;
            MoveFocus(3, kPresetRowFocusCount + 1);
            float puddle = g_oCloudThk.active.load() ? g_oCloudThk.value.load() : 0.0f;
            const SliderLaneState puddleLane = SliderLane(3, 3);
            const bool puddleResetFocused = puddleLane.resetFocused;
            const bool puddleSliderFocused = Focused(3, 3) && !puddleResetFocused;
            const bool detailFeatureAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::DetailControls);

            if (detailFeatureAvailable && puddleSliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

            bool puddlePadChanged = false;
            if (detailFeatureAvailable && puddleSliderFocused && s_sliderEditMode) {
                if (g_padCmd.left) { puddle = max(0.0f, puddle - 0.02f * g_padCmd.leftScale); puddlePadChanged = true; }
                if (g_padCmd.right) { puddle = min(1.0f, puddle + 0.02f * g_padCmd.rightScale); puddlePadChanged = true; }
            }
            if (!detailFeatureAvailable) ImGui::BeginDisabled();
            const auto puddleRow = BeginUniformSliderRow("puddle_row", "Puddle Scale");
            FocusBegin(detailFeatureAvailable && puddleSliderFocused, puddleSliderFocused && s_sliderEditMode);
            const bool puddleChanged = ImGui::SliderFloat("##slider", &puddle, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_NoInput) || puddlePadChanged;
            FocusEnd(detailFeatureAvailable && puddleSliderFocused);
            DrawUniformSliderLabel(puddleRow);
            FocusBegin(detailFeatureAvailable && puddleResetFocused);
            const bool puddleReset = DrawUniformResetButton(puddleRow) || (detailFeatureAvailable && puddleResetFocused && g_padCmd.a);
            FocusEnd(detailFeatureAvailable && puddleResetFocused);
            if (detailFeatureAvailable && puddleReset) {
                g_oCloudThk.clear();
            } else if (detailFeatureAvailable && puddleChanged) {
                g_oCloudThk.set(puddle);
            }
            if (!detailFeatureAvailable) ImGui::EndDisabled();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Experiment", nullptr, TabFlagsFor(4))) {
            activeTabSeen = 4;
            MoveFocus(4, kPresetRowFocusCount + 3);
            const bool forceClear = g_forceClear.load();
            const bool experimentFeatureAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::ExperimentControls);
            if (forceClear || !experimentFeatureAvailable) ImGui::BeginDisabled();

            ImGui::TextDisabled("Cloud");
            ImGui::Separator();

            auto DrawExperimentMultiplierRow = [&](int idx, const char* rowId, const char* label, SliderOverride& slider) {
                float value = slider.active.load() ? slider.value.load() : 1.0f;
                const SliderLaneState lane = SliderLane(4, idx);
                const bool resetFocused = lane.resetFocused;
                const bool sliderFocused = Focused(4, idx) && !resetFocused;

                if (!forceClear && experimentFeatureAvailable && sliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

                bool padChanged = false;
                if (!forceClear && sliderFocused && s_sliderEditMode) {
                    if (g_padCmd.left) { value = max(0.0f, value - 0.10f * g_padCmd.leftScale); padChanged = true; }
                    if (g_padCmd.right) { value = min(15.0f, value + 0.10f * g_padCmd.rightScale); padChanged = true; }
                }

                const auto row = BeginUniformSliderRow(rowId, label);
                FocusBegin(sliderFocused, sliderFocused && s_sliderEditMode);
                const bool changed = ImGui::SliderFloat("##slider", &value, 0.0f, 15.0f, "x%.2f", ImGuiSliderFlags_NoInput) || padChanged;
                FocusEnd(sliderFocused);
                DrawUniformSliderLabel(row);
                FocusBegin(resetFocused);
                const bool reset = DrawUniformResetButton(row) || (!forceClear && resetFocused && g_padCmd.a);
                FocusEnd(resetFocused);
                if (!forceClear && reset) {
                    slider.clear();
                }
                else if (!forceClear && changed) {
                    value = min(15.0f, max(0.0f, value));
                    if (fabsf(value - 1.0f) <= 0.001f) slider.clear();
                    else slider.set(value);
                }
                };

            auto DrawExperimentRotationRow = [&](int idx, const char* rowId, const char* label, SliderOverride& slider) {
                float value = slider.active.load() ? slider.value.load() : 1.0f;
                const SliderLaneState lane = SliderLane(4, idx);
                const bool resetFocused = lane.resetFocused;
                const bool sliderFocused = Focused(4, idx) && !resetFocused;

                if (!forceClear && experimentFeatureAvailable && sliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

                bool padChanged = false;
                if (!forceClear && sliderFocused && s_sliderEditMode) {
                    if (g_padCmd.left) { value = max(-15.0f, value - 0.10f * g_padCmd.leftScale); padChanged = true; }
                    if (g_padCmd.right) { value = min(15.0f, value + 0.10f * g_padCmd.rightScale); padChanged = true; }
                }

                const auto row = BeginUniformSliderRow(rowId, label);
                FocusBegin(sliderFocused, sliderFocused && s_sliderEditMode);
                const bool changed = ImGui::SliderFloat("##slider", &value, -15.0f, 15.0f, "%.2f", ImGuiSliderFlags_NoInput) || padChanged;
                FocusEnd(sliderFocused);
                DrawUniformSliderLabel(row);
                FocusBegin(resetFocused);
                const bool reset = DrawUniformResetButton(row) || (!forceClear && resetFocused && g_padCmd.a);
                FocusEnd(resetFocused);
                if (!forceClear && reset) {
                    slider.clear();
                }
                else if (!forceClear && changed) {
                    value = min(15.0f, max(-15.0f, value));
                    if (fabsf(value - 1.0f) <= 0.001f) slider.clear();
                    else slider.set(value);
                }
                };

            DrawExperimentMultiplierRow(3, "exp_cloud_2c_row", "2C", g_oExpCloud2C);
            DrawExperimentMultiplierRow(4, "exp_cloud_2d_row", "2D", g_oExpCloud2D);

            ImGui::Spacing();
            ImGui::TextDisabled("Atmosphere");
            ImGui::Separator();

            DrawExperimentRotationRow(5, "exp_night_sky_rotation_row", "Night Sky Rotation X [0A]", g_oExpNightSkyRot);

            if (forceClear || !experimentFeatureAvailable) ImGui::EndDisabled();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Settings", nullptr, TabFlagsFor(5))) {
            activeTabSeen = 5;
            MoveFocus(5, kPresetRowFocusCount + 2);
            bool showGuiOnStartup = g_cfg.showGuiOnStartup;
            FocusBegin(Focused(5, 3));
            bool showGuiChanged = ImGui::Checkbox("Show GUI on startup", &showGuiOnStartup);
            FocusEnd(Focused(5, 3));
            if (!showGuiChanged && Focused(5, 3) && g_padCmd.a) {
                showGuiOnStartup = !showGuiOnStartup;
                showGuiChanged = true;
            }
            if (showGuiChanged) {
                g_cfg.showGuiOnStartup = showGuiOnStartup;
                SaveConfigShowGuiOnStartup();
                GUI_SetStatus(showGuiOnStartup ? "Startup UI enabled" : "Startup UI disabled");
            }

            float uiScale = g_cfg.uiScale;
            const SliderLaneState uiScaleLane = SliderLane(5, 4);
            const bool uiScaleResetFocused = uiScaleLane.resetFocused;
            const bool uiScaleSliderFocused = Focused(5, 4) && !uiScaleResetFocused;

            if (uiScaleSliderFocused && g_padCmd.a) s_sliderEditMode = !s_sliderEditMode;

            bool uiScalePadChanged = false;
            if (uiScaleSliderFocused && s_sliderEditMode) {
                if (g_padCmd.left) { uiScale = max(kUiScaleMin, uiScale - 0.02f * g_padCmd.leftScale); uiScalePadChanged = true; }
                if (g_padCmd.right) { uiScale = min(kUiScaleMax, uiScale + 0.02f * g_padCmd.rightScale); uiScalePadChanged = true; }
            }
            const auto uiScaleRow = BeginUniformSliderRow("ui_scale_row", "UI Scale");
            FocusBegin(uiScaleSliderFocused, uiScaleSliderFocused && s_sliderEditMode);
            bool uiScaleChanged = ImGui::SliderFloat("##slider", &uiScale, kUiScaleMin, kUiScaleMax, "%.2f", ImGuiSliderFlags_NoInput) || uiScalePadChanged;
            FocusEnd(uiScaleSliderFocused);
            DrawUniformSliderLabel(uiScaleRow);
            FocusBegin(uiScaleResetFocused);
            const bool uiScaleReset = DrawUniformResetButton(uiScaleRow) || (uiScaleResetFocused && g_padCmd.a);
            FocusEnd(uiScaleResetFocused);
            if (uiScaleReset) {
                uiScale = 1.0f;
                uiScaleChanged = true;
            }
            if (uiScaleChanged) {
                uiScale = ClampUiScale(uiScale);
                if (fabsf(uiScale - g_cfg.uiScale) > 0.0001f) {
                    g_cfg.uiScale = uiScale;
                    ApplyUiScale(g_cfg.uiScale);
                    g_uiScaleConfigDirty = true;
                    g_uiScaleLastChangeMs = GetTickCount64();
                    g_uiScaleWindowResizePending = true;
                }
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
        g_uiTabIndex = activeTabSeen;
        g_uiTabSelectRequested = false;
    }

    ImGui::TextUnformatted(g_statusText);
    ImGui::End();
}

static LRESULT CALLBACK Hooked_GameWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto IsMouseInteractionMessage = [](UINT m) -> bool {
        switch (m) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
        case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
#ifdef WM_MOUSEHWHEEL
        case WM_MOUSEHWHEEL:
#endif
            return true;
        default:
            return false;
        }
    };

    if (g_menuOpen) {
        if (IsMouseInteractionMessage(msg)) {
            g_uiControllerMode.store(false);
        }
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
        if (IsBlockedInputMessage(msg))
            return 1;
    }
    if (!g_origGameWndProc)
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    return CallWindowProcA(g_origGameWndProc, hwnd, msg, wParam, lParam);
}

static bool InitImguiDx12Runtime(IDXGISwapChain3* sc3) {
    if (!sc3) return false;
    if (g_imguiReady) return true;
    ID3D12CommandQueue* initQueue = AcquireOverlayQueueRef();
    if (!initQueue) return ImGuiInitFailOnce("command queue not captured");
    if (FAILED(sc3->GetDevice(__uuidof(ID3D12Device), (void**)&g_dx12Device)) || !g_dx12Device)
    {
        initQueue->Release();
        return ImGuiInitFailOnce("GetDevice(ID3D12Device) failed");
    }
    LogActiveGpu(g_dx12Device);

    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc3->GetDesc(&desc))) return ImGuiInitFailOnce("swapchain GetDesc failed");
    g_gameHwnd = desc.OutputWindow;
    g_swapchainFormat = desc.BufferDesc.Format;
    if (g_swapchainFormat == DXGI_FORMAT_UNKNOWN)
        g_swapchainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    g_frameCount = max(1u, min(kMaxFramesInFlight, desc.BufferCount));

    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = g_frameCount;
    if (FAILED(g_dx12Device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_dx12RtvHeap))))
    {
        initQueue->Release();
        return ImGuiInitFailOnce("RTV heap creation failed");
    }
    g_rtvDescriptorSize = g_dx12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvDesc{};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 64;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_dx12Device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_dx12SrvHeap))))
    {
        initQueue->Release();
        return ImGuiInitFailOnce("SRV heap creation failed");
    }
    g_srvDescriptorSize = g_dx12Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_srvNextFree = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvStart = g_dx12RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_frameCount; ++i) {
        if (FAILED(sc3->GetBuffer(i, IID_PPV_ARGS(&g_dx12Frames[i].renderTarget))))
        {
            initQueue->Release();
            return ImGuiInitFailOnce("swapchain GetBuffer failed");
        }
        g_dx12Frames[i].rtvHandle = rtvStart;
        g_dx12Device->CreateRenderTargetView(g_dx12Frames[i].renderTarget, nullptr, g_dx12Frames[i].rtvHandle);
        rtvStart.ptr += SIZE_T(g_rtvDescriptorSize);

        if (FAILED(g_dx12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_dx12Frames[i].cmdAllocator))))
        {
            initQueue->Release();
            return ImGuiInitFailOnce("command allocator creation failed");
        }
        g_dx12Frames[i].fenceValue = 0;
    }

    if (FAILED(g_dx12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_dx12Frames[0].cmdAllocator, nullptr, IID_PPV_ARGS(&g_dx12CmdList))))
    {
        initQueue->Release();
        return ImGuiInitFailOnce("command list creation failed");
    }
    g_dx12CmdList->Close();

    if (FAILED(g_dx12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_dx12Fence))))
    {
        initQueue->Release();
        return ImGuiInitFailOnce("fence creation failed");
    }
    g_dx12FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_dx12FenceEvent) {
        initQueue->Release();
        return ImGuiInitFailOnce("fence event creation failed");
    }
    g_dx12FenceLastValue = 0;

    if (!g_origGameWndProc && g_gameHwnd) {
        g_origGameWndProc = (WNDPROC)SetWindowLongPtrA(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)Hooked_GameWndProc);
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    g_imguiBaseStyle = ImGui::GetStyle();
    g_imguiBaseStyleValid = true;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    io.Fonts->AddFontDefault();
    {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        static const ImWchar kResetGlyphRange[] = { 0x2B6E, 0x2B6E, 0 };
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\seguisym.ttf", 13.0f, &cfg, kResetGlyphRange);
    }

    ApplyUiScale(g_cfg.uiScale);

    if (!ImGui_ImplWin32_Init(g_gameHwnd)) {
        initQueue->Release();
        return ImGuiInitFailOnce("ImGui_ImplWin32_Init failed");
    }
    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = g_dx12Device;
    initInfo.CommandQueue = initQueue;
    initInfo.NumFramesInFlight = (int)g_frameCount;
    initInfo.RTVFormat = g_swapchainFormat;
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.SrvDescriptorHeap = g_dx12SrvHeap;
    initInfo.SrvDescriptorAllocFn = ImGuiSrvAlloc;
    initInfo.SrvDescriptorFreeFn = ImGuiSrvFree;
    if (!ImGui_ImplDX12_Init(&initInfo)) {
        initQueue->Release();
        return ImGuiInitFailOnce("ImGui_ImplDX12_Init failed");
    }

    g_imguiReady = true;
    g_imguiInitFailed = false;
    FreezeOverlayQueueCapture(initQueue);
    Log("[overlay] queue frozen at init=%p\n", (void*)initQueue);
    initQueue->Release();
    Log("[+] ImGui DX12 initialized\n");
    return true;
}

static void UpdateOverlayToggleState(ULONGLONG nowTick) {
    static bool s_prevMenuOpen = g_menuOpen;
    if ((GetAsyncKeyState(g_cfg.hotkeyVK) & 1) != 0) {
        g_menuOpen = !g_menuOpen;
        g_uiControllerMode.store(false);
        if (g_menuOpen) {
            ResetOverlayOpenDiagnostics();
            LogOverlayOpenSnapshot("keyboard");
            g_padButtonsPrev = 0;
            g_uiTabSelectRequested = false;
            g_uiInputSuppressUntil.store(nowTick + 120);
            g_uiConsumeUntil.store(nowTick + 220);
            g_padUiPollPrevButtons = 0;
            g_padHoldNextMs[0] = g_padHoldNextMs[1] = g_padHoldNextMs[2] = g_padHoldNextMs[3] = 0;
            g_padHoldStartMs[0] = g_padHoldStartMs[1] = g_padHoldStartMs[2] = g_padHoldStartMs[3] = 0;
        } else {
            ClearCachedOverlayDrawData();
            if (g_uiScaleConfigDirty) {
                SaveConfigUIScale();
                g_uiScaleConfigDirty = false;
            }
        }
    }
    if ((GetAsyncKeyState(g_cfg.effectToggleVK) & 1) != 0) {
        ToggleModEnabled();
    }

    bool xinputActive = false;
    if (g_pOrigXInputGetState) {
        XINPUT_STATE state{};
        xinputActive = (g_pOrigXInputGetState(0, &state) == ERROR_SUCCESS);
    }
    if (xinputActive) {
        g_dualSenseButtons = 0;
        g_dualSenseButtonsPrev = 0;
        g_dualSenseLastInputMs = 0;
    } else {
        const ULONGLONG lastNativeRawTick = g_lastDualSenseRawPacketTick.load();
        const bool nativeDualSenseRecent = (lastNativeRawTick != 0 && (nowTick - lastNativeRawTick) <= 250ULL);
        if (!nativeDualSenseRecent) {
            WORD gameInputButtons = 0;
            if (PollGameInputButtons(&gameInputButtons)) {
                ApplyOverlayControllerButtons(gameInputButtons, g_dualSenseButtonsPrev, nowTick);
            } else {
                g_dualSenseButtons = 0;
                g_dualSenseButtonsPrev = 0;
                g_dualSenseLastInputMs = 0;
            }
        }
    }

    if (s_prevMenuOpen && !g_menuOpen && g_uiScaleConfigDirty) {
        SaveConfigUIScale();
        g_uiScaleConfigDirty = false;
    }
    s_prevMenuOpen = g_menuOpen;
    if (g_uiScaleConfigDirty && (nowTick - g_uiScaleLastChangeMs >= 400ULL)) {
        SaveConfigUIScale();
        g_uiScaleConfigDirty = false;
    }
}

static bool ShouldRenderOverlayOnPresent(IDXGISwapChain3* self) {
    if (!self) return false;
    if (!g_swapChain3) return true;
    return g_swapChain3 == self;
}

static void RenderOverlayForPresent(IDXGISwapChain3* self, ULONGLONG nowTick, const char* source, int slotIndex) {
    if (!self) return;

    TouchSwapChainHookSeen(self, slotIndex);
    (void)source;
    const bool allowOverlayOnThisPresent = ShouldRenderOverlayOnPresent(self);
    if (allowOverlayOnThisPresent && !g_swapChain3) {
        self->AddRef();
        g_swapChain3 = self;
    }
    if (allowOverlayOnThisPresent && !g_imguiReady) {
        InitImguiDx12Runtime(self);
    }

    SyncMenuCursorState();

    if (allowOverlayOnThisPresent &&
        g_imguiReady &&
        g_menuOpen &&
        g_dx12CmdList) {
        ID3D12CommandQueue* queue = AcquireOverlayQueueRef();
        if (!queue) return;
        IDXGISwapChain3* activeSwapChain = g_swapChain3 ? g_swapChain3 : self;
        if (!g_overlayFirstAttemptLogged) {
            g_overlayFirstAttemptLogged = true;
            Log("[overlay] first render attempt sc=%p owner_sc=%p queue=%p frame_count=%u format=%u src=%s\n",
                (void*)self,
                (void*)g_swapChain3,
                (void*)queue,
                g_frameCount,
                (unsigned)g_swapchainFormat,
                source ? source : "unknown");
        }
        Dx12FrameCtx* frame = nullptr;
        if (RecordOverlayCommandList(activeSwapChain, nowTick, &frame) && frame) {
            if (!g_overlayFirstSubmissionLogged) {
                g_overlayFirstSubmissionLogged = true;
                Log("[overlay] command list ready frame=%u target=%p\n",
                    activeSwapChain->GetCurrentBackBufferIndex(),
                    (void*)frame->renderTarget);
            }
            ID3D12CommandList* lists[] = { g_dx12CmdList };
            queue->ExecuteCommandLists(1, lists);
            MarkOverlaySubmissionQueued(queue, *frame);
        } else if (!g_overlayFirstRecordFailureLogged) {
            g_overlayFirstRecordFailureLogged = true;
            Log("[overlay] command list skipped sc=%p owner_sc=%p queue=%p imgui=%d menu=%d frame_count=%u\n",
                (void*)self,
                (void*)g_swapChain3,
                (void*)queue,
                g_imguiReady ? 1 : 0,
                g_menuOpen ? 1 : 0,
                g_frameCount);
        }
        queue->Release();
    }
}

static HRESULT STDMETHODCALLTYPE Hooked_SwapChainPresent(IDXGISwapChain3* self, UINT syncInterval, UINT flags) {
    if (flags & DXGI_PRESENT_DO_NOT_WAIT) {
        SwapChainPresent_fn original = ResolveSwapChainPresentOriginal(self);
        if (!original) {
            CW_LOG_ONCE(s_doNotWaitOriginalMissingLogged,
                "[W] DO_NOT_WAIT Present original unresolved; skipping overlay path without invalid-call return\n");
            return S_OK;
        }
        return original(self, syncInterval, flags);
    }

    const ULONGLONG nowTick = GetTickCount64();
    UpdateOverlayToggleState(nowTick);
    RenderOverlayForPresent(self, nowTick, "Present", 8);

    SwapChainPresent_fn original = ResolveSwapChainPresentOriginal(self);
    if (!original) {
        CW_LOG_ONCE(s_presentOriginalMissingLogged,
            "[W] Present original resolution failed; returning success to avoid DXGI invalid-call crash\n");
        return S_OK;
    }
    return original(self, syncInterval, flags);
}

static HRESULT STDMETHODCALLTYPE Hooked_SwapChainResizeBuffers(
    IDXGISwapChain3* self, UINT bufferCount, UINT width, UINT height, DXGI_FORMAT newFormat, UINT swapChainFlags) {
    if (g_imguiReady) {
        ImGui_ImplDX12_InvalidateDeviceObjects();
        ShutdownImGuiRuntime(false);
    }
    SwapChainResizeBuffers_fn original = ResolveSwapChainResizeBuffersOriginal(self);
    if (!original) {
        CW_LOG_ONCE(s_resizeOriginalMissingLogged,
            "[W] ResizeBuffers original resolution failed; returning success to avoid DXGI invalid-call crash\n");
        return S_OK;
    }
    HRESULT hr = original(self, bufferCount, width, height, newFormat, swapChainFlags);
    if (SUCCEEDED(hr)) {
        if (g_swapChain3) { g_swapChain3->Release(); g_swapChain3 = nullptr; }
    }
    return hr;
}

#undef CW_LOG_ONCE

static void HookSwapChainMethods(IDXGISwapChain* sc) {
    if (!sc) return;
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) || !sc3) return;
    void** vtbl = *reinterpret_cast<void***>(sc3);
    const bool presentCreated = CreateDxgiHook(
        g_swapChainHookEntries, g_swapChainHookEntryCount, vtbl[8],
        (void*)&Hooked_SwapChainPresent, DxgiHookKind::SwapChainPresent, 8);
    const bool resizeCreated = CreateDxgiHook(
        g_swapChainHookEntries, g_swapChainHookEntryCount, vtbl[13],
        (void*)&Hooked_SwapChainResizeBuffers, DxgiHookKind::SwapChainResizeBuffers, 13);
    if (presentCreated) {
        EnableDxgiHook(g_swapChainHookEntries, g_swapChainHookEntryCount, vtbl[8], "IDXGISwapChain::Present");
    }
    if (resizeCreated) {
        EnableDxgiHook(g_swapChainHookEntries, g_swapChainHookEntryCount, vtbl[13], "IDXGISwapChain::ResizeBuffers");
    }
    sc3->Release();
}

static void CaptureQueueFromExecute(ID3D12CommandQueue* q) {
    if (!q) return;
    D3D12_COMMAND_QUEUE_DESC qd = q->GetDesc();
    if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        static bool s_execNonDirectWarned = false;
        if (!s_execNonDirectWarned) {
            s_execNonDirectWarned = true;
            Log("[W] Ignoring non-direct ExecuteCommandLists queue type=%d\n", (int)qd.Type);
        }
        return;
    }
    if (g_dx12Device) {
        ID3D12Device* qDevice = nullptr;
        if (SUCCEEDED(q->GetDevice(__uuidof(ID3D12Device), (void**)&qDevice)) && qDevice) {
            const bool sameDevice = (qDevice == g_dx12Device);
            qDevice->Release();
            if (!sameDevice) {
                static bool s_execMismatchWarned = false;
                if (!s_execMismatchWarned) {
                    s_execMismatchWarned = true;
                    Log("[W] Ignoring ExecuteCommandLists queue due to device mismatch\n");
                }
                return;
            }
        }
    }
    if (g_dx12Queue != q) {
        AcquireSRWLockExclusive(&g_dx12QueueLock);
        if (g_dx12QueueFrozen && g_dx12Queue && g_dx12Queue != q) {
            if (!g_dx12QueueFreezeLogged) {
                g_dx12QueueFreezeLogged = true;
                Log("[overlay] ignoring direct queue change after init old=%p new=%p\n", (void*)g_dx12Queue, (void*)q);
            }
            ReleaseSRWLockExclusive(&g_dx12QueueLock);
            return;
        }
        Log("[overlay] captured direct queue=%p device=%p\n", (void*)q, (void*)g_dx12Device);
        q->AddRef();
        if (g_dx12Queue) g_dx12Queue->Release();
        g_dx12Queue = q;
        ReleaseSRWLockExclusive(&g_dx12QueueLock);
    }
}

static void STDMETHODCALLTYPE Hooked_ExecuteCommandLists(
    ID3D12CommandQueue* self, UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    CaptureQueueFromExecute(self);

    if (!g_pOrigExecuteCommandLists) return;

    g_pOrigExecuteCommandLists(self, NumCommandLists, ppCommandLists);
}

static void TryInstallD3D12QueueHook() {
    if (g_d3d12QueueHookInstalled) return;

    HMODULE d3d12 = GetModuleHandleA("d3d12.dll");
    if (!d3d12) d3d12 = LoadLibraryA("d3d12.dll");
    if (!d3d12) {
        Log("[W] d3d12.dll not available yet (queue hook deferred)\n");
        return;
    }

    using D3D12CreateDevice_fn = HRESULT (WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    auto pD3D12CreateDevice = reinterpret_cast<D3D12CreateDevice_fn>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    if (!pD3D12CreateDevice) {
        Log("[W] D3D12CreateDevice export missing (queue hook deferred)\n");
        return;
    }

    ID3D12Device* dummyDevice = nullptr;
    HRESULT hrDevice = pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dummyDevice));
    if (FAILED(hrDevice) || !dummyDevice) {
        Log("[W] D3D12 dummy device creation failed (queue hook deferred) hr=0x%08X\n", (unsigned)hrDevice);
        return;
    }

    D3D12_COMMAND_QUEUE_DESC qdesc = {};
    qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qdesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qdesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qdesc.NodeMask = 0;

    ID3D12CommandQueue* dummyQueue = nullptr;
    HRESULT hrQueue = dummyDevice->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&dummyQueue));
    if (FAILED(hrQueue) || !dummyQueue) {
        Log("[W] D3D12 dummy queue creation failed (queue hook deferred) hr=0x%08X\n", (unsigned)hrQueue);
        dummyDevice->Release();
        return;
    }

    void** vtbl = *reinterpret_cast<void***>(dummyQueue);
    void* execTarget = vtbl ? vtbl[10] : nullptr;
    if (!execTarget) {
        Log("[W] D3D12 queue vtable missing ExecuteCommandLists slot\n");
        dummyQueue->Release();
        dummyDevice->Release();
        return;
    }

    if (InstallHook(execTarget, (void*)&Hooked_ExecuteCommandLists,
        (void**)&g_pOrigExecuteCommandLists, "ID3D12CommandQueue::ExecuteCommandLists", false)) {
        g_d3d12QueueHookInstalled = true;
        Log("[+] D3D12 queue hook installed execute=%p\n", execTarget);
    } else {
        Log("[W] Failed to install D3D12 queue hook execute=%p\n", execTarget);
    }

    dummyQueue->Release();
    dummyDevice->Release();
}

static HRESULT STDMETHODCALLTYPE Hooked_FactoryCreateSwapChain(
    IDXGIFactory* self, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc, IDXGISwapChain** ppSwapChain) {
    TryInstallD3D12QueueHook();
    HRESULT hr = g_pOrigFactoryCreateSwapChain(self, pDevice, pDesc, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        if (pDesc && pDesc->OutputWindow) g_gameHwnd = pDesc->OutputWindow;
        HookSwapChainMethods(*ppSwapChain);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE Hooked_Factory2CreateSwapChainForHwnd(
    IDXGIFactory2* self, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc, IDXGIOutput* pRestrictToOutput,
    IDXGISwapChain1** ppSwapChain) {
    TryInstallD3D12QueueHook();
    HRESULT hr = g_pOrigFactory2CreateSwapChainForHwnd(
        self, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
    if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
        g_gameHwnd = hWnd;
        IDXGISwapChain* sc = nullptr;
        if (SUCCEEDED((*ppSwapChain)->QueryInterface(__uuidof(IDXGISwapChain), (void**)&sc)) && sc) {
            HookSwapChainMethods(sc);
            sc->Release();
        }
    }
    return hr;
}

static void HookFactoryMethods(IDXGIFactory* factory) {
    if (!factory || g_factoryHooksInstalled) return;
    void** vt = *reinterpret_cast<void***>(factory);
    if (g_factoryHooksInstalled) return;
    bool okA = CreateDxgiHook(
        g_factoryHookEntries, g_factoryHookEntryCount, vt[10],
        (void*)&Hooked_FactoryCreateSwapChain, DxgiHookKind::FactoryCreateSwapChain, 10);

    IDXGIFactory2* factory2 = nullptr;
    bool okB = false;
    void* factory2Target = nullptr;
    if (SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory2), (void**)&factory2)) && factory2) {
        void** vt2 = *reinterpret_cast<void***>(factory2);
        factory2Target = vt2 ? vt2[15] : nullptr;
        okB = CreateDxgiHook(
            g_factoryHookEntries, g_factoryHookEntryCount, factory2Target,
            (void*)&Hooked_Factory2CreateSwapChainForHwnd, DxgiHookKind::Factory2CreateSwapChainForHwnd, 15);
    }
    if (okA) {
        g_pOrigFactoryCreateSwapChain = reinterpret_cast<FactoryCreateSwapChain_fn>(
            ResolveAnyDxgiOriginal(g_factoryHookEntries, g_factoryHookEntryCount, DxgiHookKind::FactoryCreateSwapChain));
        okA = EnableDxgiHook(
            g_factoryHookEntries, g_factoryHookEntryCount, vt[10], "IDXGIFactory::CreateSwapChain");
    }
    if (okB) {
        g_pOrigFactory2CreateSwapChainForHwnd = reinterpret_cast<Factory2CreateSwapChainForHwnd_fn>(
            ResolveAnyDxgiOriginal(g_factoryHookEntries, g_factoryHookEntryCount, DxgiHookKind::Factory2CreateSwapChainForHwnd));
        okB = EnableDxgiHook(
            g_factoryHookEntries, g_factoryHookEntryCount, factory2Target, "IDXGIFactory2::CreateSwapChainForHwnd");
    }
    if (factory2) {
        factory2->Release();
    }
    g_factoryHooksInstalled = okA || okB;
}

static HRESULT WINAPI Hooked_CreateDXGIFactory(REFIID riid, void** ppFactory) {
    HRESULT hr = g_pOrigCreateDXGIFactory(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        HookFactoryMethods((IDXGIFactory*)*ppFactory);
    return hr;
}

static HRESULT WINAPI Hooked_CreateDXGIFactory1(REFIID riid, void** ppFactory) {
    HRESULT hr = g_pOrigCreateDXGIFactory1(riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        HookFactoryMethods((IDXGIFactory*)*ppFactory);
    return hr;
}

static HRESULT WINAPI Hooked_CreateDXGIFactory2(UINT flags, REFIID riid, void** ppFactory) {
    HRESULT hr = g_pOrigCreateDXGIFactory2(flags, riid, ppFactory);
    if (SUCCEEDED(hr) && ppFactory && *ppFactory)
        HookFactoryMethods((IDXGIFactory*)*ppFactory);
    return hr;
}

static void SetupDxgiHooksStable() {
    TryInstallD3D12QueueHook();
    if (g_dxgiHooksInstalled) return;
    HMODULE dxgi = GetModuleHandleA("dxgi.dll");
    if (!dxgi) dxgi = LoadLibraryA("dxgi.dll");
    if (!dxgi) {
        Log("[W] dxgi.dll not available yet, DX12 ImGui hooks deferred\n");
        return;
    }

    void* pCreateFactory = (void*)GetProcAddress(dxgi, "CreateDXGIFactory");
    void* pCreateFactory1 = (void*)GetProcAddress(dxgi, "CreateDXGIFactory1");
    void* pCreateFactory2 = (void*)GetProcAddress(dxgi, "CreateDXGIFactory2");

    bool ok = false;
    if (pCreateFactory) ok |= InstallHook(pCreateFactory, (void*)&Hooked_CreateDXGIFactory,
        (void**)&g_pOrigCreateDXGIFactory, "CreateDXGIFactory", false);
    if (pCreateFactory1) ok |= InstallHook(pCreateFactory1, (void*)&Hooked_CreateDXGIFactory1,
        (void**)&g_pOrigCreateDXGIFactory1, "CreateDXGIFactory1", false);
    if (pCreateFactory2) ok |= InstallHook(pCreateFactory2, (void*)&Hooked_CreateDXGIFactory2,
        (void**)&g_pOrigCreateDXGIFactory2, "CreateDXGIFactory2", false);

    g_dxgiHooksInstalled = ok;
    if (ok) {
        Log("[+] DXGI hooks ready for ImGui DX12 overlay\n");
    }
}

void SetupConfiguredDxgiHooks() {
    Log("[dxgi] backend=stable\n");
    SetupDxgiHooksStable();
}

