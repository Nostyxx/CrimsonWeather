#include "pch.h"

#include "renodx_bridge.h"
#include "preset_service.h"
#include "runtime_shared.h"

#include <reshade.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(CW_WIND_ONLY)

bool RenoDxBridgeIsAddonPresent() { return false; }
bool RenoDxBridgeIsAuroraGateEnabled() { return false; }
void RenoDxBridgeSetAuroraGateEnabled(bool) {}
uint32_t RenoDxBridgeGetAuroraRegionMask() { return 0; }
void RenoDxBridgeSetAuroraRegionMask(uint32_t) {}
void RenoDxBridgeApplyPresetAuroraSettings(bool, uint32_t) {}
bool RenoDxBridgeIsCurrentRegionAllowed() { return true; }
void RenoDxBridgeOnPresent() {}
void RenoDxBridgeOnBeginEffects(
    reshade::api::effect_runtime*,
    reshade::api::command_list*,
    reshade::api::resource_view,
    reshade::api::resource_view) {}
bool RenoDxBridgeOnSetUniformValue(
    reshade::api::effect_runtime*,
    reshade::api::effect_uniform_variable,
    const void*,
    size_t) {
    return false;
}

#else

namespace {

constexpr size_t kShaderInjectFloatCount = 38;
constexpr size_t kAuroraBrightnessIndex = 33;
constexpr size_t kAuroraChanceIndex = 34;
constexpr size_t kAuroraNightSeedIndex = 35;
constexpr uint32_t kValidAuroraRegionMask = kRenoDxAuroraAllRegions;
constexpr uint32_t kDefaultAuroraRegionMask = kRenoDxAuroraAllRegions;
constexpr uint32_t kBrightness25Bits = 0x41C80000u;
constexpr float kAuroraFadeMs = 20000.0f;

std::atomic<uintptr_t> g_renodxInjectPtr{ 0 };
std::atomic<unsigned long long> g_lastProbeTick{ 0 };
std::atomic<unsigned long long> g_lastLogTick{ 0 };
std::atomic<uint32_t> g_probeLogCount{ 0 };
std::atomic<bool> g_auroraGateEnabled{ false };
std::atomic<uint32_t> g_allowedRegionMask{ kDefaultAuroraRegionMask };
std::atomic<uint32_t> g_restoreBrightnessBits{ kBrightness25Bits };
std::atomic<uint32_t> g_blendBits{ 0x3F800000u };
std::atomic<unsigned long long> g_lastBlendTick{ 0 };
std::atomic<bool> g_forcedThisSession{ false };
std::atomic<bool> g_restoreKnown{ false };

uint32_t FloatToBits(float value) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "float size");
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float BitsToFloat(uint32_t bits) {
    float value = 0.0f;
    static_assert(sizeof(bits) == sizeof(value), "float size");
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void StoreBlend(float value) {
    g_blendBits.store(FloatToBits(std::clamp(value, 0.0f, 1.0f)), std::memory_order_relaxed);
}

float LoadBlend() {
    return std::clamp(BitsToFloat(g_blendBits.load(std::memory_order_relaxed)), 0.0f, 1.0f);
}

float ClampAuroraBrightness(float value) {
    if (!std::isfinite(value)) {
        return 25.0f;
    }
    return std::clamp(value, 0.0f, 500.0f);
}

bool ContainsNoCase(const char* text, const char* needle) {
    if (!text || !needle || !needle[0]) {
        return false;
    }

    const size_t needleLen = std::strlen(needle);
    for (const char* p = text; *p; ++p) {
        if (_strnicmp(p, needle, needleLen) == 0) {
            return true;
        }
    }
    return false;
}

uint32_t SanitizeRegionMask(uint32_t mask) {
    return mask & kValidAuroraRegionMask;
}

std::filesystem::path GameBinPath() {
    wchar_t path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (len == 0 || len >= std::size(path)) {
        return {};
    }
    std::filesystem::path exe(path);
    return exe.parent_path();
}

uintptr_t ParseLastAttachedInjectionAddress(const std::filesystem::path& logPath) {
    std::ifstream in(logPath, std::ios::binary);
    if (!in) {
        return 0;
    }

    std::string line;
    uintptr_t result = 0;
    constexpr const char* marker = "mods::shader(Attached Injections: 38 at ";
    while (std::getline(in, line)) {
        const size_t pos = line.find(marker);
        if (pos == std::string::npos) {
            continue;
        }

        const size_t start = pos + std::strlen(marker);
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(line.c_str() + start, &end, 10);
        if (parsed != 0) {
            result = static_cast<uintptr_t>(parsed);
        }
    }
    return result;
}

bool MemoryLooksReadableWritable(uintptr_t address) {
    if (!address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }

    constexpr DWORD kWritable =
        PAGE_READWRITE |
        PAGE_WRITECOPY |
        PAGE_EXECUTE_READWRITE |
        PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & kWritable) != 0;
}

bool ReadInjectionSnapshot(uintptr_t address, float (&out)[kShaderInjectFloatCount]) {
    if (!MemoryLooksReadableWritable(address)) {
        return false;
    }

    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), sizeof(out));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    const float brightness = out[kAuroraBrightnessIndex];
    const float chance = out[kAuroraChanceIndex];
    const float seed = out[kAuroraNightSeedIndex];
    return std::isfinite(brightness) &&
        std::isfinite(chance) &&
        std::isfinite(seed) &&
        brightness >= 0.0f && brightness <= 500.0f &&
        chance >= 0.0f && chance <= 100.0f;
}

bool WriteAuroraBrightness(uintptr_t address, float brightness) {
    if (!MemoryLooksReadableWritable(address)) {
        return false;
    }

    __try {
        auto* values = reinterpret_cast<float*>(address);
        values[kAuroraBrightnessIndex] = ClampAuroraBrightness(brightness);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

uintptr_t LocateRenoDxInjectionBlock() {
    if (!RenoDxBridgeIsAddonPresent()) {
        return 0;
    }

    const std::filesystem::path logPath = GameBinPath() / L"ReShade.log";
    const uintptr_t parsed = ParseLastAttachedInjectionAddress(logPath);
    if (!parsed) {
        return 0;
    }

    float snapshot[kShaderInjectFloatCount]{};
    if (!ReadInjectionSnapshot(parsed, snapshot)) {
        return 0;
    }
    return parsed;
}

bool TryResolveInjectionBlock(unsigned long long now, uintptr_t& ptr) {
    ptr = g_renodxInjectPtr.load(std::memory_order_acquire);
    if (ptr) {
        return true;
    }

    if (now - g_lastProbeTick.load(std::memory_order_relaxed) < 1000) {
        return false;
    }

    g_lastProbeTick.store(now, std::memory_order_relaxed);
    ptr = LocateRenoDxInjectionBlock();
    if (!ptr) {
        return false;
    }

    g_renodxInjectPtr.store(ptr, std::memory_order_release);
    Log("[renodx-bridge] found shader injection block ptr=0x%llX\n",
        static_cast<unsigned long long>(ptr));
    return true;
}

bool IsAuroraBrightnessUniform(reshade::api::effect_runtime* runtime, reshade::api::effect_uniform_variable variable) {
    if (!runtime || variable.handle == 0) {
        return false;
    }

    char name[128] = {};
    runtime->get_uniform_variable_name(variable, name);
    if (_stricmp(name, "AuroraBrightness") != 0) {
        return false;
    }

    char effectName[256] = {};
    runtime->get_uniform_variable_effect_name(variable, effectName);
    if (effectName[0] == '\0') {
        return true;
    }
    return ContainsNoCase(effectName, "renodx") ||
        ContainsNoCase(effectName, "crimson") ||
        ContainsNoCase(effectName, "aurora");
}

bool IsRegionAllowed(int major) {
    if (major <= kPresetRegionGlobal || major >= kPresetRegionCount) {
        return false;
    }

    const uint32_t mask = g_allowedRegionMask.load(std::memory_order_relaxed);
    return (mask & (1u << static_cast<uint32_t>(major))) != 0;
}

bool IsAllowedInCurrentRegion() {
    if (!RenoDxBridgeIsAddonPresent()) {
        return true;
    }
    if (!g_regionStateValid.load(std::memory_order_relaxed)) {
        return false;
    }

    return IsRegionAllowed(g_regionMajorId.load(std::memory_order_relaxed));
}

void RememberBrightness(float value) {
    value = ClampAuroraBrightness(value);
    g_restoreBrightnessBits.store(FloatToBits(value), std::memory_order_relaxed);
    g_restoreKnown.store(true, std::memory_order_relaxed);
}

float AuroraRegionBlendMultiplier() {
    const unsigned long long now = GetTickCount64();
    const unsigned long long last = g_lastBlendTick.exchange(now, std::memory_order_relaxed);
    const float current = LoadBlend();
    const float target = IsAllowedInCurrentRegion() ? 1.0f : 0.0f;
    if (last == 0 || std::fabs(current - target) <= 0.0001f) {
        StoreBlend(target);
        return target;
    }

    const float elapsedMs = static_cast<float>(now - last);
    const float step = std::clamp(elapsedMs / kAuroraFadeMs, 0.0f, 1.0f);
    const float next = current < target
        ? std::min(target, current + step)
        : std::max(target, current - step);
    StoreBlend(next);
    return next;
}

void LogBridgeState(const char* phase, uintptr_t ptr, const float* values, bool allowed, bool wrote, bool throttledOnly) {
    const uint32_t logIndex = g_probeLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logIndex >= 32 && !throttledOnly) {
        return;
    }

    Log("[renodx-bridge] phase=%s ptr=0x%llX region=%d/%d mask=0x%02X allowed=%u auroraBrightness=%.3f auroraChance=%.3f seed=%.3f wrote=%u\n",
        phase ? phase : "?",
        static_cast<unsigned long long>(ptr),
        g_regionMajorId.load(std::memory_order_relaxed),
        g_regionLocalId.load(std::memory_order_relaxed),
        g_allowedRegionMask.load(std::memory_order_relaxed),
        allowed ? 1u : 0u,
        values[kAuroraBrightnessIndex],
        values[kAuroraChanceIndex],
        values[kAuroraNightSeedIndex],
        wrote ? 1u : 0u);
}

void RestoreAuroraBrightnessIfForced(const char* phase) {
    if (!g_forcedThisSession.load(std::memory_order_relaxed) || !g_restoreKnown.load(std::memory_order_relaxed)) {
        StoreBlend(1.0f);
        g_lastBlendTick.store(0, std::memory_order_relaxed);
        return;
    }

    const unsigned long long now = GetTickCount64();
    uintptr_t ptr = 0;
    if (!TryResolveInjectionBlock(now, ptr)) {
        g_forcedThisSession.store(false, std::memory_order_relaxed);
        StoreBlend(1.0f);
        g_lastBlendTick.store(0, std::memory_order_relaxed);
        return;
    }

    const float restoreBrightness = BitsToFloat(g_restoreBrightnessBits.load(std::memory_order_relaxed));
    const bool wrote = WriteAuroraBrightness(ptr, restoreBrightness);
    g_forcedThisSession.store(false, std::memory_order_relaxed);
    StoreBlend(1.0f);
    g_lastBlendTick.store(0, std::memory_order_relaxed);

    float values[kShaderInjectFloatCount]{};
    if (ReadInjectionSnapshot(ptr, values)) {
        LogBridgeState(phase, ptr, values, true, wrote, false);
    }
}

void ApplyAuroraRegionGate(const char* phase) {
    if (!g_auroraGateEnabled.load(std::memory_order_relaxed) || !RenoDxBridgeIsAddonPresent()) {
        return;
    }

    const unsigned long long now = GetTickCount64();
    uintptr_t ptr = 0;
    if (!TryResolveInjectionBlock(now, ptr)) {
        return;
    }

    float values[kShaderInjectFloatCount]{};
    if (!ReadInjectionSnapshot(ptr, values)) {
        g_renodxInjectPtr.store(0, std::memory_order_release);
        Log("[renodx-bridge] lost shader injection block ptr=0x%llX\n",
            static_cast<unsigned long long>(ptr));
        return;
    }

    const float blend = AuroraRegionBlendMultiplier();
    const bool allowed = blend > 0.001f;
    bool wrote = false;
    if (blend < 0.999f) {
        if (values[kAuroraBrightnessIndex] > 0.001f) {
            if (!g_forcedThisSession.load(std::memory_order_relaxed)) {
                RememberBrightness(values[kAuroraBrightnessIndex]);
            }
        }
        const float baseBrightness = g_restoreKnown.load(std::memory_order_relaxed)
            ? BitsToFloat(g_restoreBrightnessBits.load(std::memory_order_relaxed))
            : values[kAuroraBrightnessIndex];
        const float targetBrightness = ClampAuroraBrightness(baseBrightness * blend);
        if (std::fabs(values[kAuroraBrightnessIndex] - targetBrightness) > 0.001f) {
            wrote = WriteAuroraBrightness(ptr, targetBrightness);
        }
        g_forcedThisSession.store(blend < 0.999f, std::memory_order_relaxed);
    } else if (g_forcedThisSession.load(std::memory_order_relaxed) && g_restoreKnown.load(std::memory_order_relaxed)) {
        const float restoreBrightness = BitsToFloat(g_restoreBrightnessBits.load(std::memory_order_relaxed));
        if (std::fabs(values[kAuroraBrightnessIndex] - restoreBrightness) > 0.001f) {
            wrote = WriteAuroraBrightness(ptr, restoreBrightness);
        }
        g_forcedThisSession.store(false, std::memory_order_relaxed);
    }

    if (wrote || now - g_lastLogTick.load(std::memory_order_relaxed) >= 5000) {
        g_lastLogTick.store(now, std::memory_order_relaxed);
        float after[kShaderInjectFloatCount]{};
        const float* logValues = values;
        if (ReadInjectionSnapshot(ptr, after)) {
            logValues = after;
        }
        LogBridgeState(phase, ptr, logValues, allowed, wrote, !wrote);
    }
}

} // namespace

bool RenoDxBridgeIsAddonPresent() {
    const std::filesystem::path path = GameBinPath() / L"renodx-crimsondesert.addon64";
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool RenoDxBridgeIsAuroraGateEnabled() {
    return g_auroraGateEnabled.load(std::memory_order_relaxed);
}

void RenoDxBridgeSetAuroraGateEnabled(bool enabled) {
    const bool oldEnabled = g_auroraGateEnabled.exchange(enabled, std::memory_order_relaxed);
    if (!enabled) {
        RestoreAuroraBrightnessIfForced("config-disable");
        return;
    }
    if (!oldEnabled) {
        StoreBlend(1.0f);
        g_lastBlendTick.store(GetTickCount64(), std::memory_order_relaxed);
    }
    ApplyAuroraRegionGate("config-enable");
}

uint32_t RenoDxBridgeGetAuroraRegionMask() {
    return SanitizeRegionMask(g_allowedRegionMask.load(std::memory_order_relaxed));
}

void RenoDxBridgeSetAuroraRegionMask(uint32_t mask) {
    mask = SanitizeRegionMask(mask);
    g_allowedRegionMask.store(mask, std::memory_order_relaxed);
    ApplyAuroraRegionGate("config");
}

void RenoDxBridgeApplyPresetAuroraSettings(bool enabled, uint32_t mask) {
    if (!RenoDxBridgeIsAddonPresent()) {
        return;
    }
    mask = SanitizeRegionMask(mask);
    g_allowedRegionMask.store(mask, std::memory_order_relaxed);
    g_auroraGateEnabled.store(enabled, std::memory_order_relaxed);
    if (!enabled) {
        RestoreAuroraBrightnessIfForced("preset-disable");
        return;
    }
    ApplyAuroraRegionGate("preset");
}

bool RenoDxBridgeIsCurrentRegionAllowed() {
    return IsAllowedInCurrentRegion();
}

void RenoDxBridgeOnPresent() {
    ApplyAuroraRegionGate("present");
}

void RenoDxBridgeOnBeginEffects(
    reshade::api::effect_runtime*,
    reshade::api::command_list*,
    reshade::api::resource_view,
    reshade::api::resource_view) {
    ApplyAuroraRegionGate("begin-effects");
}

bool RenoDxBridgeOnSetUniformValue(
    reshade::api::effect_runtime* runtime,
    reshade::api::effect_uniform_variable variable,
    const void* data,
    size_t size) {
    if (!IsAuroraBrightnessUniform(runtime, variable)) {
        return false;
    }
    if (!g_auroraGateEnabled.load(std::memory_order_relaxed)) {
        return false;
    }
    if (data && size >= sizeof(float)) {
        float value = 0.0f;
        std::memcpy(&value, data, sizeof(value));
        RememberBrightness(value);
        const float blend = AuroraRegionBlendMultiplier();
        Log("[renodx-bridge] observed AuroraBrightness uniform set %.3f blend=%.3f\n",
            ClampAuroraBrightness(value),
            blend);
        ApplyAuroraRegionGate("uniform");
        return blend < 0.999f;
    }
    return false;
}

#endif
