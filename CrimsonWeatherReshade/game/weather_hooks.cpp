#include "pch.h"
#include "runtime_shared.h"
#include "preset_service.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <iterator>

static float ExtractScalar(__m128 v) {
    return _mm_cvtss_f32(v);
}

static __m128 PackScalar(float v) {
    return _mm_set_ss(v);
}

static constexpr float kCloudScatteringCoefficientMin = 0.00001f;

static bool CaptureMinimapGameTime(long long eventContext) {
    long long payload = 0;
    int hour = -1;
    int minute = -1;

    __try {
        if (!eventContext) {
            return false;
        }
        payload = *reinterpret_cast<long long*>(eventContext + 0x18);
        if (!payload) {
            return false;
        }
        hour = *reinterpret_cast<int*>(payload + 0x4);
        minute = *reinterpret_cast<int*>(payload + 0x8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (hour < 0 || hour > 47 || minute < 0 || minute >= 60) {
        return false;
    }

    const int hour24 = hour % 24;
    const float gameHour = static_cast<float>(hour24) + static_cast<float>(minute) / 60.0f;
    g_timeUiClockHour24.store(hour24);
    g_timeUiClockMinute.store(minute);
    g_timeUiClockHour.store(gameHour);
    g_timeUiClockValid.store(true);
    g_timeUiClockSourceValid.store(true);
    g_timeUiClockTick.store(GetTickCount64());
    return true;
}

static bool IsReadableTickPtr(uintptr_t addr, size_t bytes) {
    if (!addr || bytes == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) {
        return false;
    }

    if (mbi.State != MEM_COMMIT) {
        return false;
    }

    const DWORD mask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & mask) == 0 || (mbi.Protect & PAGE_GUARD) != 0) {
        return false;
    }

    const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const auto end = base + mbi.RegionSize;
    return addr >= base && (addr + bytes) <= end;
}

static ResolvedEnv ResolveTickEnvCurrentBuild() {
    ResolvedEnv r{};
    if (!g_pEnvManager || !*g_pEnvManager) {
        return r;
    }

    __try {
        void* envMgr = reinterpret_cast<void*>(*g_pEnvManager);
        if (!envMgr || !IsReadableTickPtr(reinterpret_cast<uintptr_t>(envMgr), sizeof(void*))) {
            return r;
        }

        auto* vt = *reinterpret_cast<uintptr_t**>(envMgr);
        if (!vt || !IsReadableTickPtr(reinterpret_cast<uintptr_t>(vt), 0x68)) {
            return r;
        }

        auto getEntity = reinterpret_cast<long long(__fastcall*)(void*)>(vt[0x60 / 8]);
        if (!getEntity || !IsReadableTickPtr(reinterpret_cast<uintptr_t>(getEntity), 16)) {
            return r;
        }

        r.entity = getEntity(envMgr);
        if (!r.entity) {
            return r;
        }

        const ptrdiff_t particleManagerOffset = g_envWeatherStateOffset + sizeof(long long);
        if (!IsReadableTickPtr(static_cast<uintptr_t>(r.entity + g_envWeatherStateOffset), sizeof(long long)) ||
            !IsReadableTickPtr(static_cast<uintptr_t>(r.entity + particleManagerOffset), sizeof(long long))) {
            return r;
        }

        r.weatherState = *reinterpret_cast<long long*>(r.entity + g_envWeatherStateOffset);
        r.particleMgr = *reinterpret_cast<long long*>(r.entity + particleManagerOffset);
        if (!r.weatherState || !IsReadableTickPtr(static_cast<uintptr_t>(r.weatherState),
                                                  g_weatherNodeContainerOffset + sizeof(long long))) {
            return r;
        }
        if (r.particleMgr && !IsReadableTickPtr(static_cast<uintptr_t>(r.particleMgr), 0x20)) {
            r.particleMgr = 0;
        }

        long long result = *reinterpret_cast<long long*>(r.weatherState + g_weatherNodeContainerOffset);
        if (!result || !IsReadableTickPtr(static_cast<uintptr_t>(result), 0x28)) {
            return r;
        }

        r.cloudNode = *reinterpret_cast<long long*>(result + 0x18);
        r.windNode = *reinterpret_cast<long long*>(result + 0x20);
        if (r.cloudNode && !IsReadableTickPtr(static_cast<uintptr_t>(r.cloudNode), CN::DUST_ADD + sizeof(float))) {
            r.cloudNode = 0;
        }
        if (r.windNode && !IsReadableTickPtr(static_cast<uintptr_t>(r.windNode), WN::CLOUD_SCROLL_Z + sizeof(float))) {
            r.windNode = 0;
        }

        r.valid = r.entity != 0 && r.weatherState != 0 && r.cloudNode != 0 && r.windNode != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = ResolvedEnv{};
    }

    return r;
}

static ResolvedEnv ResolveCustomTickEnv() {
    ResolvedEnv currentBuildEnv = ResolveTickEnvCurrentBuild();
    if (currentBuildEnv.valid) {
        return currentBuildEnv;
    }
    return ResolveEnv();
}

template <typename T>
static bool TryReadTickValue(long long base, ptrdiff_t off, T& out) {
    const uintptr_t addr = static_cast<uintptr_t>(base + off);
    if (!base || !IsReadableTickPtr(addr, sizeof(T))) {
        return false;
    }

    __try {
        out = *reinterpret_cast<T*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ReasonableRegionFloat(float value) {
    return std::isfinite(value) && fabsf(value) < 100000000.0f;
}

static float ReadRegionFloat(long long base, ptrdiff_t off) {
    float value = 0.0f;
    if (!TryReadTickValue(base, off, value) || !ReasonableRegionFloat(value)) {
        return 0.0f;
    }
    return value;
}

static int32_t ReadRegionS32(long long base, ptrdiff_t off) {
    int32_t value = 0;
    TryReadTickValue(base, off, value);
    return value;
}

struct RegionClassification {
    int majorId = 0;
    int localId = 0;
};

static RegionClassification ClassifyRegionFromGameHudIds(int areaId, int subAreaId) {
    if (areaId <= 0 || areaId == 0xFFFF) {
        return {};
    }

    // The minimap HUD passes regioninfo table indexes, not regioninfo keys.
    // These blocks come from gamedata/regioninfo.pabgh table order:
    //   0x0002..0x003F: Pailunese territory and child nodes
    //   0x0040..0x00A4: Crimson Desert / Tashkalp / Varnia / Urdavah block
    //   0x00A6..0x014C: Hernandian territory and child nodes
    //   0x014E..0x01DC: Demenissian territory and child nodes
    //   0x01DD..0x023B: Delesyian territory and child nodes
    //   0x02D8..0x0308: Abyss and Abyss nodes
    const int localId = (subAreaId > 0 && subAreaId != 0xFFFF) ? subAreaId : areaId;
    if (areaId >= 0x0002 && areaId <= 0x003F) {
        return { 4, localId }; // Pailune
    }
    if (areaId >= 0x0040 && areaId <= 0x00A4) {
        return { 5, localId }; // Crimson Desert
    }
    if (areaId >= 0x00A6 && areaId <= 0x014C) {
        return { 1, localId }; // Hernand
    }
    if (areaId >= 0x014E && areaId <= 0x01DC) {
        return { 2, localId }; // Demeniss
    }
    if (areaId >= 0x01DD && areaId <= 0x023B) {
        return { 3, localId }; // Delesyia
    }
    if (areaId >= 0x02D8 && areaId <= 0x0308) {
        return { 6, localId }; // Abyss
    }
    return {};
}

static void LogRegionHudSample(int areaId, int subAreaId, uintptr_t callerOffset, int callerKind) {
#if defined(CW_DEV_BUILD)
    static std::atomic<int> s_lastArea{ -1 };
    static std::atomic<int> s_lastSubArea{ -1 };
    static std::atomic<uintptr_t> s_lastCaller{ 0 };

    const bool changed = s_lastArea.exchange(areaId) != areaId ||
                         s_lastSubArea.exchange(subAreaId) != subAreaId ||
                         s_lastCaller.exchange(callerOffset) != callerOffset;
    if (!changed) {
        return;
    }

    const RegionClassification classified = ClassifyRegionFromGameHudIds(areaId, subAreaId);
    Log("[region-hud] area=0x%04X sub=0x%04X caller=0x%llX kind=%d -> major=%d local=0x%04X\n",
        static_cast<unsigned>(areaId & 0xFFFF),
        static_cast<unsigned>(subAreaId & 0xFFFF),
        static_cast<unsigned long long>(callerOffset),
        callerKind,
        classified.majorId,
        static_cast<unsigned>(classified.localId & 0xFFFF));
#else
    (void)areaId;
    (void)subAreaId;
    (void)callerOffset;
    (void)callerKind;
#endif
}

static RegionClassification LastStableGameHudRegion();

static RegionClassification LastStableGameHudRegion() {
    if (!g_gameRegionHudStableValid.load()) {
        return {};
    }

    const int majorId = g_gameRegionHudStableMajorId.load();
    if (majorId == 0) {
        return {};
    }
    return { majorId, g_gameRegionHudStableLocalId.load() };
}

static RegionClassification UpdateStableGameHudRegion() {
    constexpr unsigned long long kDebounceMs = 180;

    if (!g_gameRegionHudValid.load()) {
        return LastStableGameHudRegion();
    }

    const unsigned long long lastChange = g_gameRegionHudLastChangeTick.load();
    if (!lastChange || GetTickCount64() - lastChange < kDebounceMs) {
        return LastStableGameHudRegion();
    }

    const int areaId = g_gameRegionHudAreaId.load();
    const int subAreaId = g_gameRegionHudSubAreaId.load();
    const RegionClassification classified = ClassifyRegionFromGameHudIds(areaId, subAreaId);
    g_gameRegionHudPromotedUpdateCount.store(g_gameRegionHudUpdateCount.load());
    if (classified.majorId == 0) {
        return LastStableGameHudRegion();
    }

    const int oldArea = g_gameRegionHudStableAreaId.load();
    const int oldSubArea = g_gameRegionHudStableSubAreaId.load();
    const int oldMajor = g_gameRegionHudStableMajorId.load();
    if (oldArea == areaId && oldSubArea == subAreaId && oldMajor == classified.majorId) {
        return classified;
    }

    g_gameRegionHudStableValid.store(true);
    g_gameRegionHudStableAreaId.store(areaId);
    g_gameRegionHudStableSubAreaId.store(subAreaId);
    g_gameRegionHudStableMajorId.store(classified.majorId);
    g_gameRegionHudStableLocalId.store(classified.localId);
    g_gameRegionHudStableUpdateCount.fetch_add(1);

    return classified;
}

static bool WeatherTickRegionWorkNeeded() {
    if (!g_gameRegionHudValid.load()) {
        return false;
    }
    if (!g_regionStateValid.load() || !g_gameRegionHudStableValid.load()) {
        return true;
    }
    const unsigned int updates = g_gameRegionHudUpdateCount.load();
    if (updates != g_gameRegionHudPromotedUpdateCount.load()) {
        return true;
    }
    return false;
}

static void UpdateRegionState(const ResolvedEnv& env, float dt) {
    if (!env.entity) {
        g_regionStateValid.store(false);
        g_regionMajorId.store(0);
        g_regionLocalId.store(0);
        g_regionPreviousPosValid.store(false);
        return;
    }

    const float x = ReadRegionFloat(env.entity, 0xC8);
    const float y = ReadRegionFloat(env.entity, 0xCC);
    const float z = ReadRegionFloat(env.entity, 0xD0);
    if (!ReasonableRegionFloat(x) || !ReasonableRegionFloat(y) || !ReasonableRegionFloat(z)) {
        g_regionStateValid.store(false);
        g_regionMajorId.store(0);
        g_regionLocalId.store(0);
        g_regionPreviousPosValid.store(false);
        return;
    }

    RegionClassification classified = UpdateStableGameHudRegion();

    const int previousMajor = g_regionMajorId.load();
    bool likelyTeleport = false;
    if (g_regionPreviousPosValid.load()) {
        const float dx = x - g_regionPosX.load();
        const float dz = z - g_regionPosZ.load();
        likelyTeleport = (dx * dx + dz * dz) > (900.0f * 900.0f);
    }

    if (classified.majorId != 0 && previousMajor != 0 && classified.majorId != previousMajor) {
        g_regionPreviousMajorId.store(previousMajor);
        g_regionTransitionSeconds.store(likelyTeleport ? 0.0f : 6.0f);
    } else {
        const float remaining = max(0.0f, g_regionTransitionSeconds.load() - max(0.0f, dt));
        g_regionTransitionSeconds.store(remaining);
    }

    g_regionPosX.store(x);
    g_regionPosY.store(y);
    g_regionPosZ.store(z);
    g_regionSectorX.store(ReadRegionS32(env.entity, 0xE0));
    g_regionSectorZ.store(ReadRegionS32(env.entity, 0xE8));
    g_regionMajorId.store(classified.majorId);
    g_regionLocalId.store(classified.localId);
    g_regionPreviousPosValid.store(true);
    g_regionStateValid.store(true);
}

namespace WeatherTableField {
    constexpr ptrdiff_t WindSpeed = 0x138;
    constexpr ptrdiff_t AltitudeWindRatio = 0x158;
    constexpr ptrdiff_t Snow = 0x168;
    constexpr ptrdiff_t Rain = 0x16C;
    constexpr ptrdiff_t WindBlendContribution = 0x1A4;
}

namespace AtmosphereTableField {
    constexpr ptrdiff_t SunLightIntensity = 0x18;
    constexpr ptrdiff_t MoonLightIntensity = 0x20;
    constexpr ptrdiff_t RayleighScatteringColor = 0x30;
    constexpr ptrdiff_t CloudScatteringCoefficient = 0x44;
    constexpr ptrdiff_t CloudPhaseFront = 0x48;
    constexpr ptrdiff_t SunSize = 0x60;
    constexpr ptrdiff_t MoonSize = 0x6C;
    constexpr ptrdiff_t EarthAxisTilt = 0x78;
    constexpr ptrdiff_t Latitude = 0x7C;
    constexpr ptrdiff_t RayleighHeight = 0x80;
    constexpr ptrdiff_t MieScaleHeight = 0x84;
    constexpr ptrdiff_t MieAerosolDensity = 0x88;
    constexpr ptrdiff_t MieAerosolAbsorption = 0x90;
    constexpr ptrdiff_t OzoneRatio = 0x94;
    constexpr ptrdiff_t NativeFogSecondary = 0x9C;
    constexpr ptrdiff_t HeightFogBaseline = 0xA0;
    constexpr ptrdiff_t HeightFogFalloff = 0xA4;
    constexpr ptrdiff_t CloudAmount = 0xA8;
    constexpr ptrdiff_t CloudAlpha = 0xB0;
    constexpr ptrdiff_t CloudFlow = 0xB4;
    constexpr ptrdiff_t CloudHeight = 0xB8;
    constexpr ptrdiff_t CloudShape = 0xBC;
    constexpr ptrdiff_t CloudVisibleRange = 0xC0;
    constexpr ptrdiff_t CloudFadeRange = 0xC4;
    constexpr ptrdiff_t CloudDetailRatio = 0xC8;
    constexpr ptrdiff_t CloudBaseDensity = 0xD0;
    constexpr ptrdiff_t CloudBaseContrast = 0xD4;
    constexpr ptrdiff_t HighCloudAmount = 0xD8;
    constexpr ptrdiff_t MidCloudAmount = 0xDC;
    constexpr ptrdiff_t CloudVariation = 0xE4;
    constexpr ptrdiff_t VolumeFogScatterColor = 0xEC;
    constexpr ptrdiff_t MieScatterColor = 0xF0;
}

enum class RainTableMode : int {
    Native = 0,
    Slider,
    NoRain,
    ForceClear,
};

enum class SnowTableMode : int {
    Native = 0,
    Slider,
    NoSnow,
    ForceClear,
};

enum class WindTableMode : int {
    Native = 0,
    Multiplier,
    NoWind,
};

struct ComposedWeatherFields {
    uintptr_t parent = 0;
    uintptr_t child1 = 0;
    uintptr_t atmosphereSlot = 0;
    uintptr_t atmosphereNode = 0;
    float* windSpeed = nullptr;
    float* altitudeWindRatio = nullptr;
    float* snow = nullptr;
    float* rain = nullptr;
    float* windBlendContribution = nullptr;
};

static std::atomic<uint64_t> g_rainComposeCalls{ 0 };
static std::atomic<uint64_t> g_rainTableWrites{ 0 };
static std::atomic<uint64_t> g_rainTableChanges{ 0 };
static std::atomic<uint64_t> g_rainTableInvalidLayout{ 0 };
static std::atomic<int> g_rainTableLastMode{ static_cast<int>(RainTableMode::Native) };
static std::atomic<DWORD64> g_rainTableLastStatsTick{ 0 };
static std::atomic<DWORD64> g_rainTableLastInvalidLogTick{ 0 };
static std::atomic<bool> g_rainTableCaptured{ false };
static std::atomic<uint64_t> g_snowTableWrites{ 0 };
static std::atomic<uint64_t> g_snowTableChanges{ 0 };
static std::atomic<int> g_snowTableLastMode{ static_cast<int>(SnowTableMode::Native) };
static std::atomic<uint64_t> g_windTableWrites{ 0 };
static std::atomic<uint64_t> g_windTableChanges{ 0 };
static std::atomic<int> g_windTableLastMode{ static_cast<int>(WindTableMode::Native) };
static void ApplyAtmosphereTableOverrides(uintptr_t atmosphereNode);

static const char* RainTableModeName(RainTableMode mode) {
    switch (mode) {
    case RainTableMode::Slider: return "slider";
    case RainTableMode::NoRain: return "no-rain";
    case RainTableMode::ForceClear: return "force-clear";
    default: return "native";
    }
}

static RainTableMode DesiredRainTableValue(float& outValue) {
    outValue = 0.0f;
    if (!g_modEnabled.load()) return RainTableMode::Native;
    if (g_forceClear.load()) return RainTableMode::ForceClear;
    if (g_noRain.load()) return RainTableMode::NoRain;
    if (!g_oRain.active.load()) return RainTableMode::Native;

    const float value = g_oRain.value.load();
    if (!std::isfinite(value)) return RainTableMode::Native;
    outValue = min(1.0f, max(0.0f, value));
    return RainTableMode::Slider;
}

static const char* SnowTableModeName(SnowTableMode mode) {
    switch (mode) {
    case SnowTableMode::Slider: return "slider";
    case SnowTableMode::NoSnow: return "no-snow";
    case SnowTableMode::ForceClear: return "force-clear";
    default: return "native";
    }
}

static SnowTableMode DesiredSnowTableValue(float& outValue) {
    outValue = 0.0f;
    if (!g_modEnabled.load()) return SnowTableMode::Native;
    if (g_forceClear.load()) return SnowTableMode::ForceClear;
    if (g_noSnow.load()) return SnowTableMode::NoSnow;
    if (!g_oSnow.active.load()) return SnowTableMode::Native;

    const float value = g_oSnow.value.load();
    if (!std::isfinite(value)) return SnowTableMode::Native;
    outValue = min(1.0f, max(0.0f, value));
    return SnowTableMode::Slider;
}

static const char* WindTableModeName(WindTableMode mode) {
    switch (mode) {
    case WindTableMode::Multiplier: return "multiplier";
    case WindTableMode::NoWind: return "no-wind";
    default: return "native";
    }
}

static WindTableMode DesiredWindTableValue(float& outMultiplier) {
    outMultiplier = 1.0f;
    if (!g_modEnabled.load()) return WindTableMode::Native;
    if (g_noWind.load()) {
        outMultiplier = 0.0f;
        return WindTableMode::NoWind;
    }

    const float multiplier = g_windMul.load();
    if (!std::isfinite(multiplier)) return WindTableMode::Native;
    outMultiplier = min(15.0f, max(0.0f, multiplier));
    return fabsf(outMultiplier - 1.0f) > 0.0001f
        ? WindTableMode::Multiplier
        : WindTableMode::Native;
}

static bool ResolveComposedWeatherFields(long long weatherState, ComposedWeatherFields& out) {
    out = ComposedWeatherFields{};
    __try {
        out.parent = *reinterpret_cast<uintptr_t*>(weatherState + g_weatherNodeContainerOffset);
        if (!out.parent) return false;
        out.child1 = *reinterpret_cast<uintptr_t*>(out.parent + 0x18);
        if (!out.child1) return false;
        out.atmosphereSlot = out.parent + 0x20;
        out.atmosphereNode = *reinterpret_cast<uintptr_t*>(out.atmosphereSlot);
        out.windSpeed = reinterpret_cast<float*>(out.child1 + WeatherTableField::WindSpeed);
        out.altitudeWindRatio = reinterpret_cast<float*>(out.child1 + WeatherTableField::AltitudeWindRatio);
        out.snow = reinterpret_cast<float*>(out.child1 + WeatherTableField::Snow);
        out.rain = reinterpret_cast<float*>(out.child1 + WeatherTableField::Rain);
        out.windBlendContribution = reinterpret_cast<float*>(out.child1 + WeatherTableField::WindBlendContribution);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out = ComposedWeatherFields{};
        return false;
    }

}

long long __fastcall Hooked_WeatherCompose(long long weatherState, float dt) {
    if (!g_pOrigWeatherCompose) return 0;
    const long long result = g_pOrigWeatherCompose(weatherState, dt);
    g_rainComposeCalls.fetch_add(1, std::memory_order_relaxed);

    float desiredRain = 0.0f;
    float desiredSnow = 0.0f;
    float desiredWindMultiplier = 1.0f;
    const RainTableMode rainMode = DesiredRainTableValue(desiredRain);
    const SnowTableMode snowMode = DesiredSnowTableValue(desiredSnow);
    const WindTableMode windMode = DesiredWindTableValue(desiredWindMultiplier);
    ComposedWeatherFields fields{};
    if (!ResolveComposedWeatherFields(weatherState, fields)) {
        g_rainTableInvalidLayout.fetch_add(1, std::memory_order_relaxed);
        const DWORD64 now = GetTickCount64();
        DWORD64 last = g_rainTableLastInvalidLogTick.load(std::memory_order_relaxed);
        if (now - last >= 10000 &&
            g_rainTableLastInvalidLogTick.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            Log("[weather-table] layout unavailable state=%p parent=%p child1=%p count=%llu\n",
                reinterpret_cast<void*>(weatherState), reinterpret_cast<void*>(fields.parent),
                reinterpret_cast<void*>(fields.child1),
                static_cast<unsigned long long>(g_rainTableInvalidLayout.load(std::memory_order_relaxed)));
        }
        return result;
    }

    ApplyAtmosphereTableOverrides(fields.atmosphereNode);

    float nativeRain = 0.0f;
    float nativeSnow = 0.0f;
    float nativeWindSpeed = 0.0f;
    float nativeAltitudeWindRatio = 0.0f;
    float nativeWindBlendContribution = 0.0f;
    float writtenWindSpeed = 0.0f;
    float writtenAltitudeWindRatio = 0.0f;
    float writtenWindBlendContribution = 0.0f;
    bool rainWriteSucceeded = false;
    bool snowWriteSucceeded = false;
    bool windWriteSucceeded = false;
    __try {
        nativeRain = *fields.rain;
        nativeSnow = *fields.snow;
        nativeWindSpeed = *fields.windSpeed;
        nativeAltitudeWindRatio = *fields.altitudeWindRatio;
        nativeWindBlendContribution = *fields.windBlendContribution;
        if (rainMode != RainTableMode::Native) {
            *fields.rain = desiredRain;
            rainWriteSucceeded = true;
        }
        if (snowMode != SnowTableMode::Native) {
            *fields.snow = desiredSnow;
            snowWriteSucceeded = true;
        }
        if (windMode != WindTableMode::Native) {
            writtenWindSpeed = nativeWindSpeed * desiredWindMultiplier;
            writtenAltitudeWindRatio = nativeAltitudeWindRatio * desiredWindMultiplier;
            writtenWindBlendContribution = nativeWindBlendContribution * desiredWindMultiplier;
            if (std::isfinite(writtenWindSpeed) && std::isfinite(writtenAltitudeWindRatio) &&
                std::isfinite(writtenWindBlendContribution)) {
                *fields.windSpeed = writtenWindSpeed;
                *fields.altitudeWindRatio = writtenAltitudeWindRatio;
                *fields.windBlendContribution = writtenWindBlendContribution;
                windWriteSucceeded = true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_rainTableInvalidLayout.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    if (rainWriteSucceeded) {
        g_rainTableWrites.fetch_add(1, std::memory_order_relaxed);
        if (fabsf(nativeRain - desiredRain) > 0.0001f) {
            g_rainTableChanges.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (snowWriteSucceeded) {
        g_snowTableWrites.fetch_add(1, std::memory_order_relaxed);
        if (fabsf(nativeSnow - desiredSnow) > 0.0001f) {
            g_snowTableChanges.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (windWriteSucceeded) {
        g_windTableWrites.fetch_add(1, std::memory_order_relaxed);
        if (fabsf(nativeWindSpeed - writtenWindSpeed) > 0.0001f ||
            fabsf(nativeAltitudeWindRatio - writtenAltitudeWindRatio) > 0.0001f ||
            fabsf(nativeWindBlendContribution - writtenWindBlendContribution) > 0.0001f) {
            g_windTableChanges.fetch_add(1, std::memory_order_relaxed);
        }
    }

    const DWORD64 now = GetTickCount64();
    bool expectedCapture = false;
    if (g_rainTableCaptured.compare_exchange_strong(expectedCapture, true, std::memory_order_relaxed)) {
        g_rainTableLastStatsTick.store(now, std::memory_order_relaxed);
        Log("[weather-table] captured state=%p parent=%p child1=%p atmosphere=%p native={rain=%.3f snow=%.3f wind=%.3f altitude=%.3f blend=%.3f} thread=%lu\n",
            reinterpret_cast<void*>(weatherState), reinterpret_cast<void*>(fields.parent),
            reinterpret_cast<void*>(fields.child1), reinterpret_cast<void*>(fields.atmosphereNode),
            nativeRain, nativeSnow, nativeWindSpeed,
            nativeAltitudeWindRatio, nativeWindBlendContribution,
            GetCurrentThreadId());
    }

    const int previousRainMode = g_rainTableLastMode.exchange(static_cast<int>(rainMode), std::memory_order_relaxed);
    if (previousRainMode != static_cast<int>(rainMode)) {
        Log("[weather-table] rain mode %s -> %s native=%.3f written=%.3f\n",
            RainTableModeName(static_cast<RainTableMode>(previousRainMode)), RainTableModeName(rainMode),
            nativeRain, rainMode == RainTableMode::Native ? nativeRain : desiredRain);
    }
    const int previousSnowMode = g_snowTableLastMode.exchange(static_cast<int>(snowMode), std::memory_order_relaxed);
    if (previousSnowMode != static_cast<int>(snowMode)) {
        Log("[weather-table] snow mode %s -> %s native=%.3f written=%.3f\n",
            SnowTableModeName(static_cast<SnowTableMode>(previousSnowMode)), SnowTableModeName(snowMode),
            nativeSnow, snowMode == SnowTableMode::Native ? nativeSnow : desiredSnow);
    }
    const int previousWindMode = g_windTableLastMode.exchange(static_cast<int>(windMode), std::memory_order_relaxed);
    if (previousWindMode != static_cast<int>(windMode)) {
        Log("[weather-table] wind mode %s -> %s multiplier=x%.3f native={speed=%.3f altitude=%.3f blend=%.3f} written={speed=%.3f altitude=%.3f blend=%.3f}\n",
            WindTableModeName(static_cast<WindTableMode>(previousWindMode)), WindTableModeName(windMode),
            desiredWindMultiplier, nativeWindSpeed, nativeAltitudeWindRatio, nativeWindBlendContribution,
            windMode == WindTableMode::Native ? nativeWindSpeed : writtenWindSpeed,
            windMode == WindTableMode::Native ? nativeAltitudeWindRatio : writtenAltitudeWindRatio,
            windMode == WindTableMode::Native ? nativeWindBlendContribution : writtenWindBlendContribution);
    }

    if (rainMode != RainTableMode::Native || snowMode != SnowTableMode::Native ||
        windMode != WindTableMode::Native) {
        DWORD64 last = g_rainTableLastStatsTick.load(std::memory_order_relaxed);
        if (now - last >= 30000 &&
            g_rainTableLastStatsTick.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
            Log("[weather-table] stats rain={mode=%s native=%.3f written=%.3f writes=%llu changed=%llu} snow={mode=%s native=%.3f written=%.3f writes=%llu changed=%llu} wind={mode=%s multiplier=x%.3f writes=%llu changed=%llu} calls=%llu invalid=%llu\n",
                RainTableModeName(rainMode), nativeRain, desiredRain,
                static_cast<unsigned long long>(g_rainTableWrites.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_rainTableChanges.load(std::memory_order_relaxed)),
                SnowTableModeName(snowMode), nativeSnow, desiredSnow,
                static_cast<unsigned long long>(g_snowTableWrites.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_snowTableChanges.load(std::memory_order_relaxed)),
                WindTableModeName(windMode), desiredWindMultiplier,
                static_cast<unsigned long long>(g_windTableWrites.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_windTableChanges.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_rainComposeCalls.load(std::memory_order_relaxed)),
                static_cast<unsigned long long>(g_rainTableInvalidLayout.load(std::memory_order_relaxed)));
        }
    }
    return result;
}

__m128 __fastcall Hooked_GetDustIntensity(long long ws) {
    const __m128 native = g_pOrigGetDustIntensity ? g_pOrigGetDustIntensity(ws) : PackScalar(0.0f);
#if defined(CW_WIND_ONLY)
    if (!g_modEnabled.load()) {
        return native;
    }
    if (g_noWind.load())
        return PackScalar(0.0f);
    float mul = g_windMul.load();
    if (mul < 0.0f) mul = 0.0f;
    if (mul > 15.0f) mul = 15.0f;
    return PackScalar(ExtractScalar(native) * mul);
#else
    if (!g_modEnabled.load()) return native;
    if (g_forceClear.load() || g_noDust.load()) return PackScalar(0.0f);
    return native;
#endif
}

static float ThunderRateCurve(float thunder) {
    thunder = min(1.0f, max(0.0f, thunder));
    return powf(thunder, 0.55f);
}

static float CloudAmountUiToMultiplier(float ui) {
    ui = min(15.0f, max(0.0f, ui));
    if (ui <= 1.0f) {
        return ui;
    }
    return 1.0f + ((ui - 1.0f) * (2.0f / 14.0f));
}

static float CloudHeightUiToMultiplier(float ui) {
    ui = min(15.0f, max(-15.0f, ui));
    if (ui <= 0.0f) {
        return ui * (2.0f / 15.0f);
    }
    if (ui <= 1.0f) {
        return ui;
    }
    return 1.0f + ((ui - 1.0f) * (9.0f / 14.0f));
}

static unsigned int PackAtmosphereColor(float r, float g, float b, float a) {
    const auto toByte = [](float value) -> unsigned int {
        return static_cast<unsigned int>(min(1.0f, max(0.0f, value)) * 255.0f + 0.5f);
    };
    return (toByte(a) << 24) | (toByte(r) << 16) | (toByte(g) << 8) | toByte(b);
}

static float AtmosphereColorChannel(unsigned int color, unsigned int shift) {
    return static_cast<float>((color >> shift) & 0xFFu) / 255.0f;
}

static void ApplyAtmosphereTableOverrides(uintptr_t atmosphereNode) {
    if (!atmosphereNode || !g_modEnabled.load()) return;

    __try {
        const auto read = [atmosphereNode](ptrdiff_t offset) -> float {
            return *reinterpret_cast<float*>(atmosphereNode + offset);
        };
        const auto write = [atmosphereNode](ptrdiff_t offset, float value) {
            *reinterpret_cast<float*>(atmosphereNode + offset) = value;
        };
        const auto readColor = [atmosphereNode](ptrdiff_t offset) -> unsigned int {
            return *reinterpret_cast<unsigned int*>(atmosphereNode + offset);
        };
        const auto writeColor = [atmosphereNode](ptrdiff_t offset, unsigned int value) {
            *reinterpret_cast<unsigned int*>(atmosphereNode + offset) = value;
        };

        const float sunSize = read(AtmosphereTableField::SunSize);
        const float moonSize = read(AtmosphereTableField::MoonSize);
        const float earthAxisTilt = read(AtmosphereTableField::EarthAxisTilt);
        const float latitude = read(AtmosphereTableField::Latitude);
        if (!g_oSunSize.active.load() && std::isfinite(sunSize)) {
            g_atmoBaseSunSize.store(sunSize);
        }
        if (!g_oMoonSize.active.load() && std::isfinite(moonSize)) {
            g_atmoBaseMoonSize.store(moonSize);
        }
        if (!g_oExpNightSkyRot.active.load() && std::isfinite(earthAxisTilt)) {
            g_windPackBase0A.store(earthAxisTilt);
            g_windPackBase0AValid.store(true);
        }
        if (!g_oExpNightSkyRot.active.load() && std::isfinite(latitude)) {
            g_windPackBase0B.store(latitude);
            g_windPackBase0BValid.store(true);
        }
        if (std::isfinite(sunSize) && std::isfinite(moonSize)) {
            g_atmoCelestialBaseValid.store(true);
        }

        const float sunLight = read(AtmosphereTableField::SunLightIntensity);
        if (!g_oSunLightIntensity.active.load() && std::isfinite(sunLight)) {
            g_windPackBase00.store(sunLight);
            g_windPackBase00Valid.store(true);
        }
        const float moonLight = read(AtmosphereTableField::MoonLightIntensity);
        if (!g_oMoonLightIntensity.active.load() && std::isfinite(moonLight)) {
            g_windPackBase05.store(moonLight);
            g_windPackBase05Valid.store(true);
        }
        const unsigned int rayleighColor = readColor(AtmosphereTableField::RayleighScatteringColor);
        if (!g_oRayleighScatteringColor.active.load()) {
            g_windPackBase0FBits.store(rayleighColor);
            g_windPackBase0FValid.store(true);
        }
        const float rayleighHeight = read(AtmosphereTableField::RayleighHeight);
        if (!g_oRayleighHeight.active.load() && std::isfinite(rayleighHeight)) {
            g_windPackBase0E.store(rayleighHeight);
            g_windPackBase0EValid.store(true);
        }
        const float ozoneRatio = read(AtmosphereTableField::OzoneRatio);
        if (!g_oOzoneRatio.active.load() && std::isfinite(ozoneRatio)) {
            g_windPackBase14.store(ozoneRatio);
            g_windPackBase14Valid.store(true);
        }

        if (g_oSunLightIntensity.active.load() && std::isfinite(g_oSunLightIntensity.value.load())) {
            write(AtmosphereTableField::SunLightIntensity,
                  min(100.0f, max(0.0f, g_oSunLightIntensity.value.load())));
        }
        if (g_oMoonLightIntensity.active.load() && std::isfinite(g_oMoonLightIntensity.value.load())) {
            write(AtmosphereTableField::MoonLightIntensity,
                  min(100.0f, max(0.0f, g_oMoonLightIntensity.value.load())));
        }
        if (g_oRayleighScatteringColor.active.load()) {
            writeColor(AtmosphereTableField::RayleighScatteringColor,
                       PackAtmosphereColor(g_oRayleighScatteringColor.r.load(),
                                           g_oRayleighScatteringColor.g.load(),
                                           g_oRayleighScatteringColor.b.load(), 0.0f));
        }
        if (g_oRayleighHeight.active.load() && std::isfinite(g_oRayleighHeight.value.load())) {
            write(AtmosphereTableField::RayleighHeight,
                  min(200000.0f, max(1.0f, g_oRayleighHeight.value.load())));
        }
        if (g_oOzoneRatio.active.load() && std::isfinite(g_oOzoneRatio.value.load())) {
            write(AtmosphereTableField::OzoneRatio,
                  min(100.0f, max(0.0f, g_oOzoneRatio.value.load())));
        }
        if (g_oSunSize.active.load()) {
            write(AtmosphereTableField::SunSize,
                  min(10.0f, max(0.01f, g_oSunSize.value.load())));
        }
        if (g_oMoonSize.active.load()) {
            write(AtmosphereTableField::MoonSize,
                  min(100.0f, max(0.001f, g_oMoonSize.value.load())));
        }
        if (g_oExpNightSkyRot.active.load() && g_windPackBase0BValid.load()) {
            const float pitch = min(89.0f, max(-89.0f, g_oExpNightSkyRot.value.load()));
            write(AtmosphereTableField::EarthAxisTilt,
                  pitch - 90.0f + g_windPackBase0B.load());
        }

        const float mieScaleHeight = read(AtmosphereTableField::MieScaleHeight);
        if (!g_oMieScaleHeight.active.load() && std::isfinite(mieScaleHeight)) {
            g_windPackBase10.store(mieScaleHeight);
            g_windPackBase10Valid.store(true);
        }
        const float mieDensity = read(AtmosphereTableField::MieAerosolDensity);
        if (!g_oNativeFog.active.load() && !g_oMieAerosolDensity.active.load() &&
            std::isfinite(mieDensity)) {
            g_windPackBase11.store(mieDensity);
            g_windPackBase11Valid.store(true);
        }
        const float mieAbsorption = read(AtmosphereTableField::MieAerosolAbsorption);
        if (!g_oMieAerosolAbsorption.active.load() && std::isfinite(mieAbsorption)) {
            g_windPackBase12.store(mieAbsorption);
            g_windPackBase12Valid.store(true);
        }
        const float nativeFogSecondary = read(AtmosphereTableField::NativeFogSecondary);
        if (!g_oNativeFog.active.load() && std::isfinite(nativeFogSecondary)) {
            g_windPackBase17.store(nativeFogSecondary);
            g_windPackBase17Valid.store(true);
        }
        const float heightFogBaseline = read(AtmosphereTableField::HeightFogBaseline);
        if (!g_oHeightFogBaseline.active.load() && std::isfinite(heightFogBaseline)) {
            g_windPackBase18.store(heightFogBaseline);
            g_windPackBase18Valid.store(true);
        }
        const float heightFogFalloff = read(AtmosphereTableField::HeightFogFalloff);
        if (!g_oHeightFogFalloff.active.load() && std::isfinite(heightFogFalloff)) {
            g_windPackBase19.store(heightFogFalloff);
            g_windPackBase19Valid.store(true);
        }
        const unsigned int volumeFogColor = readColor(AtmosphereTableField::VolumeFogScatterColor);
        if (!g_oVolumeFogScatterColor.active.load()) {
            g_windPackBase34.store(AtmosphereColorChannel(volumeFogColor, 16));
            g_windPackBase35.store(AtmosphereColorChannel(volumeFogColor, 8));
            g_windPackBase36.store(AtmosphereColorChannel(volumeFogColor, 0));
            g_windPackBase37.store(AtmosphereColorChannel(volumeFogColor, 24));
            g_windPackBaseVolumeFogColorValid.store(true);
        }
        const unsigned int mieScatterColor = readColor(AtmosphereTableField::MieScatterColor);
        if (!g_oMieScatterColor.active.load()) {
            g_windPackBase38.store(AtmosphereColorChannel(mieScatterColor, 16));
            g_windPackBase39.store(AtmosphereColorChannel(mieScatterColor, 8));
            g_windPackBase3A.store(AtmosphereColorChannel(mieScatterColor, 0));
            g_windPackBase3B.store(AtmosphereColorChannel(mieScatterColor, 24));
            g_windPackBaseMieScatterColorValid.store(true);
        }

        const float cloudHeight = read(AtmosphereTableField::CloudHeight);
        const float cloudShape = read(AtmosphereTableField::CloudShape);
        const float highCloudAmount = read(AtmosphereTableField::HighCloudAmount);
        const float midCloudAmount = read(AtmosphereTableField::MidCloudAmount);
        if (std::isfinite(cloudHeight) && std::isfinite(cloudShape) &&
            std::isfinite(highCloudAmount) && std::isfinite(midCloudAmount) &&
            !g_windPackBaseValid.load()) {
            g_windPackBase23.store(cloudHeight);
            g_windPackBase24.store(cloudShape);
            g_windPackBase2F.store(highCloudAmount);
            g_windPackBase30.store(midCloudAmount);
            g_windPackBaseValid.store(true);
        }

        const float cloudAmount = read(AtmosphereTableField::CloudAmount);
        if (!g_oCloudAmount.active.load() && std::isfinite(cloudAmount)) {
            g_windPackBase1B.store(cloudAmount);
            g_windPackBase1BValid.store(true);
        }
        if (!g_oHighClouds.active.load() && std::isfinite(midCloudAmount)) {
            g_windPackBase30.store(midCloudAmount);
        }
        if (!g_oAtmoAlpha.active.load() && std::isfinite(highCloudAmount)) {
            g_windPackBase2F.store(highCloudAmount);
        }

        const float baseDensity = read(AtmosphereTableField::CloudBaseDensity);
        if (!g_oExpCloud2C.active.load() && std::isfinite(baseDensity)) {
            g_windPackBase2C.store(baseDensity);
            g_windPackBase2CValid.store(true);
        }
        const float baseContrast = read(AtmosphereTableField::CloudBaseContrast);
        if (!g_oExpCloud2D.active.load() && std::isfinite(baseContrast)) {
            g_windPackBase2D.store(baseContrast);
            g_windPackBase2DValid.store(true);
        }
        const float variation = read(AtmosphereTableField::CloudVariation);
        if (!g_oCloudVariation.active.load() && std::isfinite(variation)) {
            g_windPackBase32.store(variation);
            g_windPackBase32Valid.store(true);
        }

        const float cloudAlpha = read(AtmosphereTableField::CloudAlpha);
        if (!g_oCloudAlpha.active.load() && std::isfinite(cloudAlpha)) {
            g_windPackBase1E.store(cloudAlpha);
            g_windPackBase1EValid.store(true);
        }
        const float cloudFlow = read(AtmosphereTableField::CloudFlow);
        if (!g_oCloudFlow.active.load() && std::isfinite(cloudFlow)) {
            g_windPackBase1F.store(cloudFlow);
            g_windPackBase1FValid.store(true);
        }
        const float scattering = read(AtmosphereTableField::CloudScatteringCoefficient);
        if (!g_oCloudScatteringCoefficient.active.load() && std::isfinite(scattering)) {
            g_windPackBase20.store(scattering);
            g_windPackBase20Valid.store(true);
        }
        const float phaseFront = read(AtmosphereTableField::CloudPhaseFront);
        if (!g_oCloudPhaseFront.active.load() && std::isfinite(phaseFront)) {
            g_windPackBase21.store(phaseFront);
            g_windPackBase21Valid.store(true);
        }
        const float visibleRange = read(AtmosphereTableField::CloudVisibleRange);
        if (!g_oCloudVisibleRange.active.load() && std::isfinite(visibleRange)) {
            g_windPackBase25.store(visibleRange);
            g_windPackBase25Valid.store(true);
        }
        const float fadeRange = read(AtmosphereTableField::CloudFadeRange);
        if (!g_oCloudFadeRange.active.load() && std::isfinite(fadeRange)) {
            g_windPackBase27.store(fadeRange);
            g_windPackBase27Valid.store(true);
        }
        const float detailRatio = read(AtmosphereTableField::CloudDetailRatio);
        if (!g_oCloudDetailRatio.active.load() && std::isfinite(detailRatio)) {
            g_windPackBase28.store(detailRatio);
            g_windPackBase28Valid.store(true);
        }

        if (g_forceClear.load()) {
            write(AtmosphereTableField::CloudAmount, 0.0f);
            write(AtmosphereTableField::MieAerosolDensity, 0.0f);
            write(AtmosphereTableField::NativeFogSecondary, 0.0f);
            return;
        }

        const bool noFog = g_noFog.load();
        if (noFog) {
            write(AtmosphereTableField::MieAerosolDensity, 0.0f);
            write(AtmosphereTableField::NativeFogSecondary, 0.0f);
        }

        if ((g_oCloudSpdX.active.load() || g_oCloudSpdY.active.load()) && g_windPackBaseValid.load()) {
            const float heightMul = CloudHeightUiToMultiplier(g_oCloudSpdX.get(1.0f));
            const float shapeMul = min(10.0f, max(0.0f, g_oCloudSpdY.get(1.0f)));
            write(AtmosphereTableField::CloudHeight, g_windPackBase23.load() * heightMul);
            write(AtmosphereTableField::CloudShape, g_windPackBase24.load() * shapeMul);
        }
        if (g_oCloudAmount.active.load() && g_windPackBase1BValid.load()) {
            const float mul = CloudAmountUiToMultiplier(g_oCloudAmount.get(1.0f));
            write(AtmosphereTableField::CloudAmount,
                  min(3.0f, max(0.0f, g_windPackBase1B.load() * mul)));
        }
        if (g_oHighClouds.active.load() && g_windPackBaseValid.load()) {
            const float mul = min(15.0f, max(0.0f, g_oHighClouds.get(1.0f)));
            write(AtmosphereTableField::MidCloudAmount, g_windPackBase30.load() * mul);
        }
        if (g_oAtmoAlpha.active.load() && g_windPackBaseValid.load()) {
            const float mul = min(15.0f, max(0.0f, g_oAtmoAlpha.get(1.0f)));
            write(AtmosphereTableField::HighCloudAmount, g_windPackBase2F.load() * mul);
        }
        if (g_oExpCloud2C.active.load() && g_windPackBase2CValid.load()) {
            const float mul = min(15.0f, max(0.0f, g_oExpCloud2C.get(1.0f)));
            write(AtmosphereTableField::CloudBaseDensity, g_windPackBase2C.load() * mul);
        }
        if (g_oExpCloud2D.active.load() && g_windPackBase2DValid.load()) {
            const float mul = min(15.0f, max(0.0f, g_oExpCloud2D.get(1.0f)));
            write(AtmosphereTableField::CloudBaseContrast, g_windPackBase2D.load() * mul);
        }
        if (g_oCloudVariation.active.load() && g_windPackBase32Valid.load()) {
            const float mul = min(15.0f, max(0.0f, g_oCloudVariation.get(1.0f)));
            write(AtmosphereTableField::CloudVariation, g_windPackBase32.load() * mul);
        }
        if (g_oCloudAlpha.active.load()) {
            write(AtmosphereTableField::CloudAlpha,
                  min(100.0f, max(0.0f, g_oCloudAlpha.value.load())));
        }
        if (g_oCloudScatteringCoefficient.active.load()) {
            write(AtmosphereTableField::CloudScatteringCoefficient,
                  min(100.0f, max(kCloudScatteringCoefficientMin,
                                  g_oCloudScatteringCoefficient.value.load())));
        }
        if (g_oCloudFlow.active.load()) {
            write(AtmosphereTableField::CloudFlow,
                  min(50.0f, max(0.0f, g_oCloudFlow.value.load())));
        }
        if (g_oCloudVisibleRange.active.load() && g_windPackBase25Valid.load()) {
            const float mul = min(10.0f, max(0.0f, g_oCloudVisibleRange.value.load()));
            write(AtmosphereTableField::CloudVisibleRange, g_windPackBase25.load() * mul);
        }
        if (g_oCloudPhaseFront.active.load()) {
            write(AtmosphereTableField::CloudPhaseFront,
                  min(1.0f, max(-1.0f, g_oCloudPhaseFront.value.load())));
        }
        if (g_oCloudFadeRange.active.load()) {
            write(AtmosphereTableField::CloudFadeRange,
                  min(200000.0f, max(0.0f, g_oCloudFadeRange.value.load())));
        }
        if (g_oCloudDetailRatio.active.load()) {
            write(AtmosphereTableField::CloudDetailRatio,
                  min(1.5f, max(0.0f, g_oCloudDetailRatio.value.load())));
        }

        if (!noFog && g_oNativeFog.active.load()) {
            const float fogMul = 1.0f + min(15.0f, max(0.0f, g_oNativeFog.value.load()));
            if (g_windPackBase11Valid.load()) {
                write(AtmosphereTableField::MieAerosolDensity, g_windPackBase11.load() * fogMul);
            }
            if (g_windPackBase17Valid.load()) {
                write(AtmosphereTableField::NativeFogSecondary, g_windPackBase17.load() * fogMul);
            }
        }
        if (!noFog) {
            if (g_oMieScaleHeight.active.load()) {
                write(AtmosphereTableField::MieScaleHeight,
                      min(200000.0f, max(1.0f, g_oMieScaleHeight.value.load())));
            }
            if (g_oMieAerosolDensity.active.load()) {
                write(AtmosphereTableField::MieAerosolDensity,
                      min(100.0f, max(0.0f, g_oMieAerosolDensity.value.load())));
            }
            if (g_oMieAerosolAbsorption.active.load()) {
                write(AtmosphereTableField::MieAerosolAbsorption,
                      min(100.0f, max(0.0f, g_oMieAerosolAbsorption.value.load())));
            }
            if (g_oHeightFogBaseline.active.load()) {
                write(AtmosphereTableField::HeightFogBaseline,
                      min(50000.0f, max(-50000.0f, g_oHeightFogBaseline.value.load())));
            }
            if (g_oHeightFogFalloff.active.load()) {
                write(AtmosphereTableField::HeightFogFalloff,
                      min(100.0f, max(0.0f, g_oHeightFogFalloff.value.load())));
            }
            if (g_oVolumeFogScatterColor.active.load()) {
                writeColor(AtmosphereTableField::VolumeFogScatterColor,
                           PackAtmosphereColor(g_oVolumeFogScatterColor.r.load(),
                                               g_oVolumeFogScatterColor.g.load(),
                                               g_oVolumeFogScatterColor.b.load(),
                                               g_oVolumeFogScatterColor.a.load()));
            }
            if (g_oMieScatterColor.active.load()) {
                writeColor(AtmosphereTableField::MieScatterColor,
                           PackAtmosphereColor(g_oMieScatterColor.r.load(),
                                               g_oMieScatterColor.g.load(),
                                               g_oMieScatterColor.b.load(),
                                               g_oMieScatterColor.a.load()));
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[W] atmosphere table override exception node=%p\n",
            reinterpret_cast<void*>(atmosphereNode));
    }
}

static constexpr float kDegToRad = 0.01745329251994329577f;
static constexpr float kRadToDeg = 57.295779513082320876f;
static constexpr int kSceneTimeW = 3;
static constexpr int kSceneSunDirection = 42 * 4;
static constexpr int kSceneMoonDirection = 43 * 4;
static constexpr int kSceneMoonRight = 44 * 4;
static constexpr int kSceneMoonUp = 45 * 4;

struct CwVec3 {
    float x;
    float y;
    float z;
};

static float ClampFloat(float v, float lo, float hi) {
    return min(hi, max(lo, v));
}

static unsigned int FloatBits(float value) {
    unsigned int bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float NormalizeSignedDegrees(float v) {
    while (v > 180.0f) v -= 360.0f;
    while (v < -180.0f) v += 360.0f;
    return v;
}

static float SceneTimeToNightSkyYaw(float sceneTimeW) {
    if (!std::isfinite(sceneTimeW)) return 0.0f;
    return NormalizeSignedDegrees((sceneTimeW * 15.0f) - 180.0f);
}

static float NightSkyYawToSceneTime(float yaw) {
    return (NormalizeSignedDegrees(yaw) + 180.0f) / 15.0f;
}

static CwVec3 NormalizeVec3(CwVec3 v, CwVec3 fallback) {
    const float lenSq = v.x * v.x + v.y * v.y + v.z * v.z;
    if (!std::isfinite(lenSq) || lenSq < 0.000001f) return fallback;
    const float invLen = 1.0f / sqrtf(lenSq);
    return { v.x * invLen, v.y * invLen, v.z * invLen };
}

static CwVec3 CrossVec3(CwVec3 a, CwVec3 b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

static void DirectionToYawPitch(CwVec3 dir, float& yaw, float& pitch) {
    dir = NormalizeVec3(dir, { 0.0f, 0.0f, 1.0f });
    yaw = atan2f(dir.x, dir.z) * kRadToDeg;
    pitch = asinf(ClampFloat(dir.y, -1.0f, 1.0f)) * kRadToDeg;
}

static CwVec3 YawPitchToDirection(float yaw, float pitch) {
    const float yawRad = yaw * kDegToRad;
    const float pitchRad = ClampFloat(pitch, -89.0f, 89.0f) * kDegToRad;
    const float cp = cosf(pitchRad);
    return NormalizeVec3({ sinf(yawRad) * cp, sinf(pitchRad), cosf(yawRad) * cp }, { 0.0f, 0.0f, 1.0f });
}

static void StoreFloat4Direction(float* scene, int index, CwVec3 dir) {
    scene[index + 0] = dir.x;
    scene[index + 1] = dir.y;
    scene[index + 2] = dir.z;
}

static void StoreMoonBasis(float* scene, CwVec3 moonDir, float rollDegrees) {
    const CwVec3 upRef = fabsf(moonDir.y) > 0.98f ? CwVec3{ 1.0f, 0.0f, 0.0f } : CwVec3{ 0.0f, 1.0f, 0.0f };
    CwVec3 right = NormalizeVec3(CrossVec3(upRef, moonDir), { 1.0f, 0.0f, 0.0f });
    CwVec3 up = NormalizeVec3(CrossVec3(moonDir, right), { 0.0f, 1.0f, 0.0f });

    if (std::isfinite(rollDegrees) && fabsf(rollDegrees) > 0.0001f) {
        const float rollRad = rollDegrees * kDegToRad;
        const float c = cosf(rollRad);
        const float s = sinf(rollRad);
        const CwVec3 rolledRight{
            right.x * c + up.x * s,
            right.y * c + up.y * s,
            right.z * c + up.z * s
        };
        const CwVec3 rolledUp{
            up.x * c - right.x * s,
            up.y * c - right.y * s,
            up.z * c - right.z * s
        };
        right = NormalizeVec3(rolledRight, right);
        up = NormalizeVec3(rolledUp, up);
    }

    StoreFloat4Direction(scene, kSceneMoonRight, right);
    StoreFloat4Direction(scene, kSceneMoonUp, up);
}

static void CaptureSceneCelestialBase(const float* scene) {
    if (!scene) return;
    CwVec3 sunDir{ scene[kSceneSunDirection + 0], scene[kSceneSunDirection + 1], scene[kSceneSunDirection + 2] };
    CwVec3 moonDir{ scene[kSceneMoonDirection + 0], scene[kSceneMoonDirection + 1], scene[kSceneMoonDirection + 2] };
    if (!std::isfinite(sunDir.x) || !std::isfinite(sunDir.y) || !std::isfinite(sunDir.z) ||
        !std::isfinite(moonDir.x) || !std::isfinite(moonDir.y) || !std::isfinite(moonDir.z)) {
        return;
    }

    float sunYaw = 0.0f, sunPitch = 0.0f, moonYaw = 0.0f, moonPitch = 0.0f;
    DirectionToYawPitch(sunDir, sunYaw, sunPitch);
    DirectionToYawPitch(moonDir, moonYaw, moonPitch);
    if (!g_oSunDirX.active.load()) {
        g_sceneBaseSunYaw.store(sunYaw);
    }
    if (!g_oSunDirY.active.load()) {
        g_sceneBaseSunPitch.store(sunPitch);
    }
    if (!g_oMoonDirX.active.load()) {
        g_sceneBaseMoonYaw.store(moonYaw);
    }
    if (!g_oMoonDirY.active.load()) {
        g_sceneBaseMoonPitch.store(moonPitch);
    }
    if (!g_oNightSkyYaw.active.load() && std::isfinite(scene[kSceneTimeW])) {
        g_sceneBaseNightSkyYaw.store(SceneTimeToNightSkyYaw(scene[kSceneTimeW]));
    }
    if (!g_sceneCelestialBaseValid.load()) {
        g_sceneCelestialBaseValid.store(true);
    }
}

static bool AnySceneCelestialOverrideActive() {
    return g_oSunDirX.active.load() || g_oSunDirY.active.load() ||
           g_oMoonDirX.active.load() || g_oMoonDirY.active.load() ||
           g_oMoonRoll.active.load() || g_oNightSkyYaw.active.load();
}

static bool ApplySceneCelestialOverrides(float* scene) {
    if (!scene || !AnySceneCelestialOverrideActive()) return false;
    CaptureSceneCelestialBase(scene);
    if (!g_sceneCelestialBaseValid.load()) return false;

    if (g_oSunDirX.active.load() || g_oSunDirY.active.load()) {
        const float yaw = g_oSunDirX.active.load() ? g_oSunDirX.value.load() : g_sceneBaseSunYaw.load();
        const float pitch = g_oSunDirY.active.load() ? g_oSunDirY.value.load() : g_sceneBaseSunPitch.load();
        StoreFloat4Direction(scene, kSceneSunDirection, YawPitchToDirection(yaw, pitch));
    }
    if (g_oMoonDirX.active.load() || g_oMoonDirY.active.load() || g_oMoonRoll.active.load()) {
        const float yaw = g_oMoonDirX.active.load() ? g_oMoonDirX.value.load() : g_sceneBaseMoonYaw.load();
        const float pitch = g_oMoonDirY.active.load() ? g_oMoonDirY.value.load() : g_sceneBaseMoonPitch.load();
        const CwVec3 moonDir = YawPitchToDirection(yaw, pitch);
        if (g_oMoonDirX.active.load() || g_oMoonDirY.active.load()) {
            StoreFloat4Direction(scene, kSceneMoonDirection, moonDir);
        }
        const float roll = g_oMoonRoll.active.load() ? ClampFloat(g_oMoonRoll.value.load(), -180.0f, 180.0f) : 0.0f;
        StoreMoonBasis(scene, moonDir, roll);
    }
    if (g_oNightSkyYaw.active.load()) {
        scene[kSceneTimeW] = NightSkyYawToSceneTime(g_oNightSkyYaw.value.load());
    }

    return true;
}

void* __fastcall Hooked_SceneFrameUpdate(long long self, long long context) {
    void* result = g_pOrigSceneFrameUpdate ? g_pOrigSceneFrameUpdate(self, context) : nullptr;
    if (!g_modEnabled.load() || !AnySceneCelestialOverrideActive() || !self ||
        !g_sceneFrameSourceOffset || !g_sceneFrameOwnerOffset) {
        return result;
    }

    __try {
        auto* sceneSource = *reinterpret_cast<float**>(self + g_sceneFrameSourceOffset);
        if (sceneSource) {
            ApplySceneCelestialOverrides(sceneSource);
        }

        auto* sceneOwner = *reinterpret_cast<uint8_t**>(self + g_sceneFrameOwnerOffset);
        if (!sceneOwner) {
            return result;
        }
        auto* scenePrimary = *reinterpret_cast<float**>(sceneOwner + 0x20);
        ApplySceneCelestialOverrides(scenePrimary);
        auto* sceneCopy = *reinterpret_cast<float**>(sceneOwner + 0x60);
        ApplySceneCelestialOverrides(sceneCopy);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[W] celestial scene override exception\n");
    }
    return result;
}

// Apply all weather parameters after the engine tick.
static void ApplyWeatherParams(long long self, const ResolvedEnv& env) {
    if (!env.valid) return;
    if (env.cloudNode) {
        if (g_oCloudThk.active.load()) {
            float cThk = Clamp01(g_oCloudThk.get(0.0f));
            At<float>(env.cloudNode, CN::CLOUD_THICK) = cThk;

            if (cThk <= 0.0001f) {
                static bool s_dryPulse = false;
                s_dryPulse = !s_dryPulse;
                At<float>(env.cloudNode, CN::CLOUD_TOP)   = s_dryPulse ? 0.0005f : 0.0f;
                At<float>(env.cloudNode, CN::CLOUD_BASE)  = 0.0f;
            }
        }

    }

    (void)self;
}
static uint32_t ComputeCustomEffectMask() {
    uint32_t mask = 0;
    float rain = (!g_noRain.load() && g_oRain.active.load()) ? g_oRain.value.load() : 0.0f;
    float snow = (!g_noSnow.load() && g_oSnow.active.load()) ? g_oSnow.value.load() : 0.0f;
    float dust = (!g_noDust.load() && g_oDust.active.load()) ? g_oDust.value.load() : 0.0f;
    float wind = g_oWindActual.active.load() ? g_oWindActual.value.load() : 0.0f;

    if (rain > 0.01f) mask |= 0x003;      // effects 0,1 (rain drops)
    if (rain > 0.5f)  mask |= 0x010;      // effect 4 (heavy rain)
    if (snow > 0.01f) mask |= 0x004;      // effect 2 (snow flakes)
    if (snow > 0.3f)  mask |= 0x008;      // effect 3 (heavy snow)
    if (wind > 0.5f)  mask |= 0x020;      // effect 5 (legacy wind/heavy-weather lane)
    if (dust > 0.1f)  mask |= 0x040;      // effect 6 (sand dust)

    return mask;
}

static bool IsRainOnlyControlMode() {
    bool rainDriven = !g_noRain.load() && g_oRain.active.load();
    bool others = g_noSnow.load() || g_noDust.load() || g_oSnow.active.load() || g_oDust.active.load() ||
                   g_oCloudThk.active.load() ||
                   g_oCloudSpdX.active.load() || g_oCloudSpdY.active.load() ||
                   g_oHighClouds.active.load() ||
                   g_oAtmoAlpha.active.load() ||
                   g_oWindActual.active.load();
    return rainDriven && !others;
}

static void StopAllWeatherEffects(long long self) {
    if (!g_pSetIntensity || !g_pNullSentinel) return;
    const ResolvedEnv env = ResolveEnv();
    if (!env.valid || !env.particleMgr) return;
    const int nullSent = *g_pNullSentinel;
    for (int i = 0; i < kEffectCount; i++) {
        int h = At<int>(self, WCO::HANDLE_ARRAY + i * 4);
        if (h != nullSent) {
            g_pSetIntensity(env.particleMgr, h, 0.0f);
        }
    }
}

static void StopWeatherEffectsByMask(long long self, uint32_t effectMask) {
    if (!g_pSetIntensity || !g_pNullSentinel) return;
    const ResolvedEnv env = ResolveEnv();
    if (!env.valid || !env.particleMgr) return;
    const int nullSent = *g_pNullSentinel;
    for (int i = 0; i < kEffectCount; i++) {
        if ((effectMask & (1u << i)) == 0) continue;
        int h = At<int>(self, WCO::HANDLE_ARRAY + i * 4);
        if (h != nullSent) g_pSetIntensity(env.particleMgr, h, 0.0f);
    }
}

static std::atomic<int> g_snowEffectCleanupTicks{ 0 };
static std::atomic<bool> g_snowEffectWasWanted{ false };

static uint32_t ComputeSuppressedWeatherEffectMask() {
    uint32_t mask = 0;
    if (g_noRain.load()) mask |= 0x013u;
    if (g_noSnow.load()) mask |= 0x00Cu;
    if (g_noDust.load()) mask |= 0x040u;
    return mask;
}

static void ApplyNoWindPolicy(long long self, const ResolvedEnv& env) {
    (void)env;
    At<int>(self, WCO::SOUND_WIND)    = 0;
    At<int>(self, WCO::SOUND_SKYWIND) = 0;
}

static void ApplyWindFromSlider(long long self, const ResolvedEnv& env) {
    if (!env.valid) return;
    if (!g_oWindActual.active.load()) return;

    float wSpd = max(0.0f, g_oWindActual.value.load());
    if (env.windNode) {
        At<float>(env.windNode, WN::SPEED) = wSpd;
    }

    if (!g_pActivateEffect || !g_pSetIntensity || !g_pNullSentinel || !env.particleMgr) return;
    const int nullSent = *g_pNullSentinel;
    int& handle = At<int>(self, WCO::HANDLE_ARRAY + 5 * 4); // effect 5 = wind base
    if (handle == nullSent) {
        const EffectSlot& s = kSlots[5];
        g_pActivateEffect(self, s.id,
            reinterpret_cast<long long*>(self + s.slotA),
            reinterpret_cast<long long*>(self + s.slotB), 1.0f);
    }
    int h = At<int>(self, WCO::HANDLE_ARRAY + 5 * 4);
    if (h != nullSent) {
        g_pSetIntensity(env.particleMgr, h, min(1.0f, wSpd / 10.0f));
    }
}

// Per-tick update after the engine tick.
static void TickWeatherState(long long self, float dt) {
    (void)dt;
    if (g_activeWeather != kCustomWeather) return;
    if (!AnyCustomWeatherSliderActive()) {
        StopAllWeatherEffects(self);
        g_activeWeather = -1;
        return;
    }

    const ResolvedEnv env = ResolveCustomTickEnv();
    if (!env.valid) return;
    const int nullSent = g_pNullSentinel ? *g_pNullSentinel : 0;

    if (IsRainOnlyControlMode()) {
        return;
    }

    ApplyWeatherParams(self, env);
    const uint32_t mask = ComputeCustomEffectMask();

    if (g_pActivateEffect && g_pSetIntensity && env.particleMgr) {
        for (int i = 0; i < kEffectCount; ++i) {
            // Rain slots are owned by the original WeatherTick, which consumes
            // the finalized weather-table value through the native getter.
            if (i == 0 || i == 1 || i == 4) {
                continue;
            }
            int& handle = At<int>(self, WCO::HANDLE_ARRAY + i * 4);
            if (mask & (1u << i)) {
                const EffectSlot& slot = kSlots[i];
                if (handle == nullSent) {
                    g_pActivateEffect(self, slot.id,
                        reinterpret_cast<long long*>(self + slot.slotA),
                        reinterpret_cast<long long*>(self + slot.slotB), 1.0f);
                }
                const int activeHandle = At<int>(self, WCO::HANDLE_ARRAY + i * 4);
                if (activeHandle != nullSent) {
                    float intensity = 1.0f;
                    if (i <= 1) intensity = (!g_noRain.load() && g_oRain.active.load()) ? g_oRain.value.load() : 0.0f;
                    else if (i == 2 || i == 3) intensity = (!g_noSnow.load() && g_oSnow.active.load()) ? g_oSnow.value.load() : 0.0f;
                    else if (i == 4) intensity = (!g_noRain.load() && g_oRain.active.load()) ? max(0.0f, g_oRain.value.load() - 0.5f) * 2.0f : 0.0f;
                    else if (i == 5) intensity = g_oWindActual.active.load() ? min(1.0f, g_oWindActual.value.load() / 10.0f) : 0.0f;
                    else if (i == 6) intensity = (!g_noDust.load() && g_oDust.active.load()) ? min(1.0f, g_oDust.value.load()) : 0.0f;
                    else if (i == 7) intensity = min(
                        (!g_noSnow.load() && g_oSnow.active.load()) ? g_oSnow.value.load() : 0.0f,
                        (!g_noDust.load() && g_oDust.active.load()) ? g_oDust.value.load() : 0.0f);
                    else if (i == 8) intensity = 0.0f;
                    g_pSetIntensity(env.particleMgr, activeHandle, intensity);
                }
            } else if (handle != nullSent) {
                g_pSetIntensity(env.particleMgr, handle, 0.0f);
            }
        }
    }
}

static int DeactivateWeatherEffectsByMask(long long self, uint32_t effectMask) {
    if (!g_pNullSentinel) return 0;
    const int nullSent = *g_pNullSentinel;
    const ResolvedEnv env = ResolveEnv();
    int deactivated = 0;

    for (int i = 0; i < kEffectCount; ++i) {
        if ((effectMask & (1u << i)) == 0) continue;
        int& handle = At<int>(self, WCO::HANDLE_ARRAY + i * 4);
        if (handle == nullSent) continue;

        if (g_pSetIntensity && env.valid && env.particleMgr) {
            g_pSetIntensity(env.particleMgr, handle, 0.0f);
        }
        if (g_pDeactivateEffect) {
            __try {
                g_pDeactivateEffect(&handle);
                handle = nullSent;
                ++deactivated;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("[weather-effects] native deactivation exception slot=%d handle=%d\n", i, handle);
            }
        }
    }
    return deactivated;
}

static bool SnowEffectWantedNow() {
    return !g_forceClear.load() &&
           !g_noSnow.load() &&
           g_oSnow.active.load() &&
           g_oSnow.value.load() > 0.01f;
}

static void UpdateSnowEffectTransitionCleanup() {
    const bool wanted = SnowEffectWantedNow();
    const bool wasWanted = g_snowEffectWasWanted.exchange(wanted);
    if (wasWanted && !wanted) {
        constexpr int kSnowCleanupTicks = 30;
        g_snowEffectCleanupTicks.store(kSnowCleanupTicks);
        Log("[snow] cleanup requested: snow transitioned off ticks=%d\n", kSnowCleanupTicks);
    }
}

static bool SnowEffectCleanupActive() {
    return g_snowEffectCleanupTicks.load() > 0;
}

static void TickSnowEffectCleanup(long long self) {
    int ticks = g_snowEffectCleanupTicks.load();
    if (ticks <= 0) {
        return;
    }

    const int deactivated = DeactivateWeatherEffectsByMask(self, 0x00Cu);
    if (deactivated > 0) {
        Log("[snow] released native effect handle(s)=%d\n", deactivated);
    }
    ticks = g_snowEffectCleanupTicks.fetch_sub(1) - 1;
    if (ticks <= 0) {
        g_snowEffectCleanupTicks.store(0);
        Log("[snow] cleanup finished\n");
    }
}

static void EnterCustomMode() {
    if (g_activeWeather != kCustomWeather) {
        g_activeWeather = kCustomWeather;
        Log("[Custom] Entered custom slider mode\n");
        GUI_SetStatus("Custom (sliders)");
    }
}

enum class MinimapRegionCallerKind : int {
    Unknown = 0,
    Setup = 1,
    RefreshCurrent = 2,
    EventCallback = 3,
    RegionNode = 4,
};

static MinimapRegionCallerKind ClassifyMinimapRegionCaller(uintptr_t callerOffset) {
    if (callerOffset >= 0xB2CD50 && callerOffset < 0xB2D8E1) {
        return MinimapRegionCallerKind::Setup;
    }
    if (callerOffset >= 0xB2EEC0 && callerOffset < 0xB2EF70) {
        return MinimapRegionCallerKind::RefreshCurrent;
    }
    if (callerOffset >= 0xB3BB10 && callerOffset < 0xB3BB2B) {
        return MinimapRegionCallerKind::EventCallback;
    }
    if (callerOffset >= 0xA4D2530 && callerOffset < 0xA4D2663) {
        return MinimapRegionCallerKind::RegionNode;
    }
    return MinimapRegionCallerKind::Unknown;
}

long long __fastcall Hooked_MinimapRegionLabels(long long self, unsigned short areaId, unsigned short subAreaId) {
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    long long result = 0;
    if (g_pOrigMinimapRegionLabels) {
        result = g_pOrigMinimapRegionLabels(self, areaId, subAreaId);
    }

    const auto moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uintptr_t callerOffset = moduleBase && caller >= moduleBase ? caller - moduleBase : 0;
    const MinimapRegionCallerKind callerKind = ClassifyMinimapRegionCaller(callerOffset);

    g_gameRegionHudAreaId.exchange(static_cast<int>(areaId));
    g_gameRegionHudSubAreaId.exchange(static_cast<int>(subAreaId));
    g_gameRegionHudValid.store(areaId != 0xFFFF || subAreaId != 0xFFFF);
    g_gameRegionHudCallerOffset.store(callerOffset);
    g_gameRegionHudCallerKind.store(static_cast<int>(callerKind));
    g_gameRegionHudLastChangeTick.store(GetTickCount64());
    g_gameRegionHudUpdateCount.fetch_add(1);
    LogRegionHudSample(static_cast<int>(areaId), static_cast<int>(subAreaId), callerOffset, static_cast<int>(callerKind));

    return result;
}

void __fastcall Hooked_MinimapGameTimeUpdate(long long self, long long eventContext) {
    CaptureMinimapGameTime(eventContext);

    if (g_pOrigMinimapGameTimeUpdate) {
        g_pOrigMinimapGameTimeUpdate(self, eventContext);
    }
    FlushQueuedNativeToast(reinterpret_cast<void*>(self));
}

namespace {

constexpr unsigned short kNativeGameClockRate = 12;
constexpr long long kNativeClockDayMs = 75600000ll;
constexpr long long kNativeClockJumpThresholdMs = 300000ll;

struct GameClockCalendar {
    int day = -1;
    int hour = -1;
    int minute = -1;
    int second = -1;
    int millisecond = -1;
};

bool ReadGameClockCalendarStruct(long long outTime, GameClockCalendar& out) {
    if (!outTime) {
        return false;
    }

    __try {
        const int day = *reinterpret_cast<int*>(outTime + 0x00);
        const int hour = *reinterpret_cast<int*>(outTime + 0x04);
        const int minute = *reinterpret_cast<int*>(outTime + 0x08);
        const int second = *reinterpret_cast<int*>(outTime + 0x0C);
        const int millisecond = *reinterpret_cast<int*>(outTime + 0x10);
        if (day < 0 || hour < 0 || hour > 47 || minute < 0 || minute > 59 ||
            second < 0 || second > 59 || millisecond < 0 || millisecond > 999) {
            return false;
        }
        out.day = day;
        out.hour = hour;
        out.minute = minute;
        out.second = second;
        out.millisecond = millisecond;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

long long CalendarToNativeClockTicks(const GameClockCalendar& cal) {
    const long long extraDays = cal.hour >= 24 ? (cal.hour / 24) : 0;
    const long long hour = cal.hour >= 24 ? (cal.hour % 24) : cal.hour;
    const unsigned long long visualMs =
        static_cast<unsigned long long>(cal.millisecond) +
        1000ull * (static_cast<unsigned long long>(cal.second) +
        60ull * (static_cast<unsigned long long>(cal.minute) +
        60ull * static_cast<unsigned long long>(hour)));

    unsigned long long withinDay = visualMs;
    if (visualMs >= 10800000ull) {
        withinDay = visualMs < 75600000ull
            ? visualMs - 5400000ull
            : (visualMs + 64800000ull) >> 1;
    } else {
        withinDay = visualMs >> 1;
    }
    return static_cast<long long>(withinDay +
        static_cast<unsigned long long>(kNativeClockDayMs) *
        static_cast<unsigned long long>(cal.day + extraDays));
}

GameClockCalendar NativeClockTicksToCalendar(long long clockTicks) {
    GameClockCalendar out;
    if (clockTicks < 0) {
        clockTicks = 0;
    }

    const unsigned long long ticks = static_cast<unsigned long long>(clockTicks);
    const unsigned long long withinDay = ticks % static_cast<unsigned long long>(kNativeClockDayMs);
    out.day = static_cast<int>(ticks / static_cast<unsigned long long>(kNativeClockDayMs));
    const unsigned long long visualMs = withinDay >= 5400000ull
        ? (withinDay >= 70200000ull ? (2ull * withinDay) - 64800000ull : withinDay + 5400000ull)
        : 2ull * withinDay;
    const unsigned long long totalSeconds = visualMs / 1000ull;
    out.hour = static_cast<int>((totalSeconds / 3600ull) % 24ull);
    out.minute = static_cast<int>((totalSeconds / 60ull) % 60ull);
    out.second = static_cast<int>(totalSeconds % 60ull);
    out.millisecond = static_cast<int>(visualMs % 1000ull);
    return out;
}

void WriteGameClockCalendarStruct(long long outTime, const GameClockCalendar& cal) {
    if (!outTime) {
        return;
    }

    __try {
        *reinterpret_cast<int*>(outTime + 0x00) = cal.day;
        *reinterpret_cast<int*>(outTime + 0x04) = cal.hour;
        *reinterpret_cast<int*>(outTime + 0x08) = cal.minute;
        *reinterpret_cast<int*>(outTime + 0x0C) = cal.second;
        *reinterpret_cast<int*>(outTime + 0x10) = cal.millisecond;
        *reinterpret_cast<unsigned char*>(outTime + 0x14) = static_cast<unsigned char>((cal.day % 7 + 7) % 7);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void WriteGameClockSnapshotStruct(long long storage,
                                  const GameClockCalendar& cal,
                                  unsigned short rate,
                                  long long clockTimestampMs) {
    if (!storage) {
        return;
    }

    __try {
        *reinterpret_cast<int*>(storage + 0x00) = cal.day;
        *reinterpret_cast<int*>(storage + 0x04) = cal.hour;
        *reinterpret_cast<int*>(storage + 0x08) = cal.minute;
        *reinterpret_cast<int*>(storage + 0x0C) = cal.second;
        *reinterpret_cast<int*>(storage + 0x10) = cal.millisecond;
        *reinterpret_cast<unsigned char*>(storage + 0x14) = static_cast<unsigned char>((cal.day % 7 + 7) % 7);
        *reinterpret_cast<unsigned short*>(storage + 0x16) = rate ? rate : 1;
        *reinterpret_cast<long long*>(storage + 0x18) = clockTimestampMs;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

struct SnapshotClockControllerState {
    SRWLOCK lock = SRWLOCK_INIT;
    bool active = false;
    bool anchorValid = false;
    uintptr_t lastStorage = 0;
    long long virtualTick = 0;
    long long lastNativeTick = 0;
    long long lastTimestamp = 0;
    double fractionalTicks = 0.0;
    float lastRequestedScale = 0.0f;
    unsigned long long calls = 0;
    unsigned long long lastHealthLogMs = 0;
};

SnapshotClockControllerState g_snapshotClockController;
std::atomic<bool> g_snapshotClockControllerActive{ false };

bool CurrentGameClockUsesTlsSnapshot() {
    __try {
        const auto tlsArray = reinterpret_cast<void**>(__readgsqword(0x58));
        if (!tlsArray) {
            return false;
        }
        const auto gameTls = *reinterpret_cast<const unsigned char* const*>(tlsArray);
        return gameTls && g_gameClockTlsFlagOffset && gameTls[g_gameClockTlsFlagOffset] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ResolveCurrentGameClockSnapshot(unsigned char* source,
                                     uintptr_t& outStorage,
                                     bool& outField,
                                     bool& outTls,
                                     unsigned short& outAreaId) {
    outStorage = 0;
    outField = false;
    outTls = CurrentGameClockUsesTlsSnapshot();
    outAreaId = 0xFFFF;

    if (source && g_pGameFieldInfoResolver && g_gameClockFieldStorageOffset &&
        g_gameClockFieldEnabledOffset) {
        __try {
            void** vtable = *reinterpret_cast<void***>(source);
            if (vtable && vtable[11]) {
                using GetAreaIdFn = void(__fastcall*)(unsigned char*, unsigned short*);
                reinterpret_cast<GetAreaIdFn>(vtable[11])(source, &outAreaId);
                if (outAreaId != 0xFFFF) {
                    const auto fieldInfo = static_cast<uintptr_t>(g_pGameFieldInfoResolver(&outAreaId));
                    if (fieldInfo && *reinterpret_cast<unsigned char*>(fieldInfo + g_gameClockFieldEnabledOffset)) {
                        outStorage = fieldInfo + g_gameClockFieldStorageOffset;
                        outField = true;
                        return true;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            outAreaId = 0xFFFF;
        }
    }

    outStorage = outTls ? g_addrGameClockSnapshotTls : g_addrGameClockSnapshotPrimary;
    return outStorage != 0;
}

float RequestedGameClockScale(const GameClockCalendar& cal) {
    const bool isDay = cal.hour >= 3 && cal.hour < 19;
    const float scale = isDay ? g_realGameTimeDayScale.load() : g_realGameTimeNightScale.load();
    return min(60.0f, max(0.01f, scale));
}

bool CommitVirtualGameClockToNative(unsigned char* source,
                                    long long outTime,
                                    const GameClockCalendar& nativeCal,
                                    const GameClockCalendar& targetCal) {
    if (!outTime || !g_pGameTimeSetter || !g_pGameTimeManagerRootGlobal ||
        !g_gameTimeManagerOffset) {
        return false;
    }

    __try {
        const uintptr_t root = *g_pGameTimeManagerRootGlobal;
        const uintptr_t manager = root
            ? *reinterpret_cast<const uintptr_t*>(root + g_gameTimeManagerOffset)
            : 0;
        if (!manager) {
            return false;
        }

        alignas(32) unsigned char committedClock[0x20] = {};
        memcpy(committedClock, reinterpret_cast<const void*>(outTime), sizeof(committedClock));
        const unsigned short nativeRate = *reinterpret_cast<const unsigned short*>(outTime + 0x16);
        const long long timestamp = *reinterpret_cast<const long long*>(outTime + 0x18);
        WriteGameClockSnapshotStruct(reinterpret_cast<long long>(committedClock), targetCal,
            nativeRate ? nativeRate : kNativeGameClockRate, timestamp);

        g_pGameTimeSetter(static_cast<long long>(manager),
            reinterpret_cast<long long>(committedClock), 1, 0, 0, 0);
        WriteGameClockCalendarStruct(outTime, targetCal);

        unsigned char* managerSource = nullptr;
        const auto sourceHolder = *reinterpret_cast<unsigned char***>(manager + 0x20);
        if (sourceHolder) {
            managerSource = *sourceHolder;
        }
        Log("[real-time] native-commit manager=%p source=%p managerSource=%p native=day %d %02d:%02d:%02d.%03d target=day %d %02d:%02d:%02d.%03d rate=%u timestamp=%lld\n",
            reinterpret_cast<void*>(manager), source, managerSource,
            nativeCal.day, nativeCal.hour, nativeCal.minute, nativeCal.second, nativeCal.millisecond,
            targetCal.day, targetCal.hour, targetCal.minute, targetCal.second, targetCal.millisecond,
            static_cast<unsigned>(nativeRate ? nativeRate : kNativeGameClockRate), timestamp);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ApplyVirtualGameClockController(unsigned char* source,
                                     long long outTime,
                                     GameClockCalendar& nativeCal) {
    if (!outTime || !g_addrGameClockSnapshotPrimary || !g_addrGameClockSnapshotTls ||
        !g_gameClockTlsFlagOffset) {
        return false;
    }

    uintptr_t storage = 0;
    bool fieldStorage = false;
    bool tlsStorage = false;
    unsigned short areaId = 0xFFFF;
    if (!ResolveCurrentGameClockSnapshot(source, storage, fieldStorage, tlsStorage, areaId)) {
        return false;
    }

    AcquireSRWLockExclusive(&g_snapshotClockController.lock);
    bool modified = false;
    bool commitOnExit = false;
    GameClockCalendar commitNativeCal;
    GameClockCalendar commitTargetCal;
    __try {
        const bool enabled = g_modEnabled.load() && g_realGameTimeEnabled.load();
        const long long timestamp = *reinterpret_cast<long long*>(outTime + 0x18);

        if (!enabled) {
            if (g_snapshotClockController.active) {
                commitNativeCal = nativeCal;
                commitTargetCal = NativeClockTicksToCalendar(g_snapshotClockController.virtualTick);
                commitOnExit = true;
            }
            g_snapshotClockController.active = false;
            g_snapshotClockControllerActive.store(false, std::memory_order_release);
            g_snapshotClockController.anchorValid = false;
            g_snapshotClockController.lastStorage = 0;
            g_snapshotClockController.fractionalTicks = 0.0;
            g_snapshotClockController.lastRequestedScale = 0.0f;
            __leave;
        }

        // TLS is a separate simulation context that can lag the player clock by days.
        if (tlsStorage) {
            __leave;
        }

        const GameClockCalendar sourceCal = nativeCal;
        const long long nativeTick = CalendarToNativeClockTicks(sourceCal);
        if (!g_snapshotClockController.active || !g_snapshotClockController.anchorValid) {
            g_snapshotClockController.active = true;
            g_snapshotClockControllerActive.store(true, std::memory_order_release);
            g_snapshotClockController.anchorValid = true;
            g_snapshotClockController.virtualTick = nativeTick;
            g_snapshotClockController.lastNativeTick = nativeTick;
            g_snapshotClockController.lastTimestamp = timestamp;
            g_snapshotClockController.fractionalTicks = 0.0;
            g_snapshotClockController.lastStorage = storage;
            Log("[real-time] controller activated source=%s storage=%p day=%d %02d:%02d:%02d.%03d timestamp=%lld\n",
                fieldStorage ? "field" : "primary",
                reinterpret_cast<void*>(storage),
                sourceCal.day, sourceCal.hour, sourceCal.minute, sourceCal.second, sourceCal.millisecond,
                timestamp);
        }

        if (!fieldStorage) {
            const long long timestampDelta = timestamp >= g_snapshotClockController.lastTimestamp
                ? timestamp - g_snapshotClockController.lastTimestamp
                : 0;
            const long long nativeDelta = nativeTick - g_snapshotClockController.lastNativeTick;
            const long long expectedNativeDelta = timestampDelta * static_cast<long long>(kNativeGameClockRate);
            const long long nativeCorrection = nativeDelta - expectedNativeDelta;

            GameClockCalendar virtualCal = NativeClockTicksToCalendar(g_snapshotClockController.virtualTick);
            const float requestedScale = RequestedGameClockScale(virtualCal);
            const double scaledTicks =
                static_cast<double>(expectedNativeDelta) * static_cast<double>(requestedScale) +
                g_snapshotClockController.fractionalTicks;
            const long long wholeTicks = static_cast<long long>(std::floor(scaledTicks));
            g_snapshotClockController.fractionalTicks = scaledTicks - static_cast<double>(wholeTicks);
            g_snapshotClockController.virtualTick = max(0ll,
                g_snapshotClockController.virtualTick + wholeTicks);

            if (llabs(nativeCorrection) >= kNativeClockJumpThresholdMs) {
                g_snapshotClockController.virtualTick = max(0ll,
                    g_snapshotClockController.virtualTick + nativeCorrection);
                Log("[real-time] native clock jump synchronized delta=%lld correction=%lld native=day %d %02d:%02d:%02d.%03d\n",
                    nativeDelta,
                    nativeCorrection,
                    sourceCal.day, sourceCal.hour, sourceCal.minute, sourceCal.second, sourceCal.millisecond);
            }

            g_snapshotClockController.lastNativeTick = nativeTick;
            g_snapshotClockController.lastTimestamp = timestamp;

            if (fabsf(g_snapshotClockController.lastRequestedScale - requestedScale) > 0.0001f) {
                const unsigned long long writes = g_realGameTimeScaleWriteCount.fetch_add(1) + 1;
                Log("[real-time] virtual-rate source=primary requested=x%.4f nativeRate=%u day=%d %02d:%02d writes=%llu\n",
                    requestedScale,
                    static_cast<unsigned>(kNativeGameClockRate),
                    virtualCal.day, virtualCal.hour, virtualCal.minute,
                    writes);
                g_snapshotClockController.lastRequestedScale = requestedScale;
            }
        }

        int dayDelta = 0;
        int minuteRequest = -1;
        dayDelta = g_realGameTimeDayDeltaRequest.exchange(0);
        minuteRequest = g_realGameTimeSetMinuteRequest.exchange(-1);
        GameClockCalendar target = NativeClockTicksToCalendar(g_snapshotClockController.virtualTick);
        if (dayDelta != 0) {
            target.day = max(0, target.day + dayDelta);
        }
        if (minuteRequest >= 0) {
            const int clampedMinute = min(24 * 60 - 1, max(0, minuteRequest));
            target.hour = clampedMinute / 60;
            target.minute = clampedMinute % 60;
            target.second = 0;
            target.millisecond = 0;
        }
        const bool actionRequested = dayDelta != 0 || minuteRequest >= 0;
        if (actionRequested) {
            g_snapshotClockController.virtualTick = CalendarToNativeClockTicks(target);
            g_snapshotClockController.fractionalTicks = 0.0;
        }

        WriteGameClockCalendarStruct(outTime, target);
        nativeCal = target;
        modified = true;

        if (actionRequested) {
            const unsigned long long writes = g_realGameTimeWriteCount.fetch_add(1) + 1;
            Log("[real-time] virtual-action action=%s source=%s area=%u target=day %d %02d:%02d:%02d.%03d writes=%llu\n",
                dayDelta ? "day-step" : "clock-set",
                fieldStorage ? "field" : "primary",
                static_cast<unsigned>(areaId),
                target.day, target.hour, target.minute, target.second, target.millisecond,
                writes);
        }

        ++g_snapshotClockController.calls;
        const unsigned long long now = GetTickCount64();
        if (!g_snapshotClockController.lastHealthLogMs ||
            now - g_snapshotClockController.lastHealthLogMs >= 10000ull) {
            const float requestedScale = RequestedGameClockScale(target);
            Log("[real-time] virtual-health source=%s native=day %d %02d:%02d virtual=day %d %02d:%02d scale=x%.4f calls=%llu\n",
                fieldStorage ? "field" : "primary",
                sourceCal.day, sourceCal.hour, sourceCal.minute,
                target.day, target.hour, target.minute,
                requestedScale,
                g_snapshotClockController.calls);
            g_snapshotClockController.lastHealthLogMs = now;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        modified = false;
        commitOnExit = false;
    }
    ReleaseSRWLockExclusive(&g_snapshotClockController.lock);
    if (commitOnExit) {
        if (CommitVirtualGameClockToNative(source, outTime, commitNativeCal, commitTargetCal)) {
            nativeCal = commitTargetCal;
            modified = true;
            Log("[real-time] controller deactivated; virtual clock committed to native storage\n");
        } else {
            Log("[W] Real game clock native commit failed; passthrough may return to the pre-override time\n");
        }
    }
    return modified;
}

void StoreGameTimeGetterSample(unsigned char* source, long long outTime) {
    // The TLS snapshot belongs to a separate simulation context and can lag the
    // primary world clock by days. It must not repaint the user-facing clock UI.
    if (CurrentGameClockUsesTlsSnapshot()) {
        return;
    }

    constexpr unsigned long long kSampleIntervalMs = 50;
    const unsigned long long now = GetTickCount64();
    unsigned long long lastRealtimeMs = g_gameTimeProbeLastGetterRealtimeMs.load(std::memory_order_relaxed);
    if (lastRealtimeMs && now - lastRealtimeMs < kSampleIntervalMs) {
        return;
    }
    if (!g_gameTimeProbeLastGetterRealtimeMs.compare_exchange_strong(
            lastRealtimeMs, now, std::memory_order_relaxed)) {
        return;
    }

    GameClockCalendar cal;
    if (!ReadGameClockCalendarStruct(outTime, cal)) {
        return;
    }

    const unsigned long long calls = g_gameTimeProbeGetterCallCount.fetch_add(1) + 1;
    const unsigned long long elapsedRealtimeMs = (lastRealtimeMs && now >= lastRealtimeMs) ? (now - lastRealtimeMs) : 0;

    g_gameTimeProbeLastGetterFrameMs.store(elapsedRealtimeMs);
    g_gameTimeProbeDay.store(cal.day);
    g_gameTimeProbeHour.store(cal.hour);
    g_gameTimeProbeMinute.store(cal.minute);
    g_gameTimeProbeSecond.store(cal.second);
    g_gameTimeProbeMillisecond.store(cal.millisecond);

    (void)source;
    (void)calls;
}

} // namespace

void ResetGameTimeProbeStats() {
    g_gameTimeProbeCallCount.store(0);
    g_gameTimeProbeGetterCallCount.store(0);
    g_gameTimeProbeLastRealtimeMs.store(0);
    g_gameTimeProbeLastGetterRealtimeMs.store(0);
    g_gameTimeProbeLastFrameMs.store(0);
    g_gameTimeProbeLastGetterFrameMs.store(0);
    g_gameTimeProbeLastClockMs.store(0);
    g_gameTimeProbeLastDeltaClockMs.store(0);
    g_gameTimeProbeLastAccumUs.store(0);
    g_gameTimeProbeLastDeltaAccumUs.store(0);
    g_gameTimeProbeAccumulatorActive.store(false);
    g_gameTimeProbeLastSpeed.store(0.0f);
    g_gameTimeProbeDay.store(-1);
    g_gameTimeProbeHour.store(-1);
    g_gameTimeProbeMinute.store(-1);
    g_gameTimeProbeSecond.store(-1);
    g_gameTimeProbeMillisecond.store(-1);
    g_gameTimeProbeLastMinuteDelta.store(0);
    g_gameTimeProbeLastMinuteRealtimeMs.store(0);
    g_gameTimeProbeMinuteChangeCount.store(0);
    g_gameTimeProbeMinuteDeltaMinMs.store(0);
    g_gameTimeProbeMinuteDeltaMaxMs.store(0);
    g_gameTimeProbeMinuteDeltaTotalMs.store(0);
}


long long __fastcall Hooked_GameTimeGetter(unsigned char* source, long long outTime) {
    long long result = outTime;
    if (g_pOrigGameTimeGetter) {
        result = g_pOrigGameTimeGetter(source, outTime);
    }

    const bool realTimeEnabled = g_modEnabled.load(std::memory_order_relaxed) &&
        g_realGameTimeEnabled.load(std::memory_order_relaxed);
    const bool controllerActive = g_snapshotClockControllerActive.load(std::memory_order_acquire);
    if (!realTimeEnabled && !controllerActive) {
        return result;
    }

    GameClockCalendar nativeCal;
    if (ReadGameClockCalendarStruct(outTime, nativeCal)) {
        if (realTimeEnabled || controllerActive) {
            ApplyVirtualGameClockController(source, outTime, nativeCal);
        }
        if (realTimeEnabled) {
            StoreGameTimeGetterSample(source, outTime);
        }
    }
    return result;
}

struct ThunderAudioCandidate {
    uint32_t eventId;
    const char* eventName;
};

struct ThunderSoundBankCandidate {
    uint32_t bankId;
    const char* bankName;
};

static void EnsureThunderSoundBanksLoaded() {
    if (!g_pAkLoadBankById) {
        return;
    }

    static bool s_attempted = false;
    if (s_attempted) {
        return;
    }
    s_attempted = true;

    constexpr ThunderSoundBankCandidate kThunderBanks[] = {
        { 3452312330u, "env_thunder_2d" },
        { 3469090149u, "env_thunder_3d" },
    };

    for (const ThunderSoundBankCandidate& bank : kThunderBanks) {
        int result = 0;
        __try {
            result = g_pAkLoadBankById(bank.bankId, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[thunder-audio] bank exception name=%s id=%u\n",
                bank.bankName, bank.bankId);
            continue;
        }
        Log("[thunder-audio] bank=%s id=%u load=%d\n",
            bank.bankName, bank.bankId, result);
    }
}

static uint64_t ResolveWeatherAudioGameObjectId() {
    if (!g_pEnvManager || !*g_pEnvManager) {
        return 0;
    }

    __try {
        const long long envMgr = static_cast<long long>(*g_pEnvManager);
        if (!envMgr || !IsReadableTickPtr(static_cast<uintptr_t>(envMgr), sizeof(uintptr_t))) {
            return 0;
        }

        auto* envVt = *reinterpret_cast<uintptr_t**>(envMgr);
        if (!envVt || !IsReadableTickPtr(reinterpret_cast<uintptr_t>(envVt), 0x68)) {
            return 0;
        }

        auto getEntity = reinterpret_cast<long long(__fastcall*)(long long)>(envVt[0x60 / 8]);
        const long long entity = getEntity(envMgr);
        if (!entity || !IsReadableTickPtr(static_cast<uintptr_t>(entity + 0x1C8), sizeof(uintptr_t))) {
            return 0;
        }

        const long long audioProvider = *reinterpret_cast<long long*>(entity + 0x1C8);
        if (!audioProvider || !IsReadableTickPtr(static_cast<uintptr_t>(audioProvider), sizeof(uintptr_t))) {
            return 0;
        }

        auto* providerVt = *reinterpret_cast<uintptr_t**>(audioProvider);
        if (!providerVt || !IsReadableTickPtr(reinterpret_cast<uintptr_t>(providerVt), 0x198)) {
            return 0;
        }

        auto getAudioObject = reinterpret_cast<long long(__fastcall*)(long long)>(providerVt[0x190 / 8]);
        const long long audioObject = getAudioObject(audioProvider);
        if (!audioObject || !IsReadableTickPtr(static_cast<uintptr_t>(audioObject + 0x18), sizeof(uint32_t))) {
            return 0;
        }

        return static_cast<uint64_t>(*reinterpret_cast<uint32_t*>(audioObject + 0x18));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[thunder-audio] audio object resolve exception\n");
        return 0;
    }
}

static void TryPlayThunderAudio() {
    if (!g_pPlayWeatherSoundEvent && !g_pAkPostEventById) {
        static DWORD64 s_lastUnavailableLog = 0;
        const DWORD64 now = GetTickCount64();
        if (now - s_lastUnavailableLog >= 10000) {
            s_lastUnavailableLog = now;
            Log("[thunder-audio] unavailable: native and AK event players missing\n");
        }
        return;
    }

    EnsureThunderSoundBanksLoaded();

    static DWORD64 s_lastThunderSound = 0;
    const DWORD64 now = GetTickCount64();
    if (now - s_lastThunderSound < 500) {
        return;
    }

    constexpr ThunderAudioCandidate kThunderAudioCandidates[] = {
        { 3742816565u, "env_2d_oneshot_thunder" },
        { 3685435772u, "env_oneshot_thunder" },
    };

    if (g_pPlayWeatherSoundEvent) {
        for (const ThunderAudioCandidate& candidate : kThunderAudioCandidates) {
            uint32_t playingId = 0;
            __try {
                playingId = static_cast<uint32_t>(g_pPlayWeatherSoundEvent(candidate.eventId));
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("[thunder-audio] native post exception name=%s id=%u\n",
                    candidate.eventName, candidate.eventId);
                continue;
            }
            if (playingId) {
                Log("[thunder-audio] native post ok event=%s id=%u playing=%u\n",
                    candidate.eventName, candidate.eventId, playingId);
                s_lastThunderSound = now;
                return;
            }
        }
    }

    if (!g_pAkPostEventById) {
        return;
    }

    const uint64_t gameObjectId = ResolveWeatherAudioGameObjectId();
    if (!gameObjectId) {
        static DWORD64 s_lastMissingObjectLog = 0;
        if (now - s_lastMissingObjectLog >= 5000) {
            s_lastMissingObjectLog = now;
            Log("[thunder-audio] skipped, weather audio object unavailable\n");
        }
        return;
    }

    for (const ThunderAudioCandidate& candidate : kThunderAudioCandidates) {
        uint32_t playingId = 0;
        __try {
            playingId = g_pAkPostEventById(candidate.eventId, gameObjectId, 0, nullptr, nullptr, 0, nullptr, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[thunder-audio] post exception name=%s id=%u object=%llu\n",
                candidate.eventName, candidate.eventId, static_cast<unsigned long long>(gameObjectId));
            continue;
        }
        if (playingId) {
            static unsigned int s_successfulPosts = 0;
            static DWORD64 s_lastSuccessLog = 0;
            ++s_successfulPosts;
            if (s_successfulPosts <= 3 || now - s_lastSuccessLog >= 60000) {
                s_lastSuccessLog = now;
                Log("[thunder-audio] post ok event=%s id=%u object=%llu playing=%u count=%u\n",
                    candidate.eventName, candidate.eventId,
                    static_cast<unsigned long long>(gameObjectId), playingId, s_successfulPosts);
            }
            s_lastThunderSound = now;
            break;
        }

        static DWORD64 s_lastZeroPlayingLog = 0;
        if (now - s_lastZeroPlayingLog >= 10000) {
            s_lastZeroPlayingLog = now;
            Log("[thunder-audio] post returned zero event=%s id=%u object=%llu\n",
                candidate.eventName, candidate.eventId,
                static_cast<unsigned long long>(gameObjectId));
        }
    }
}

static void TickNativeLightningBridge(long long self, float dt, long long weatherState) {
    constexpr float kThunderSchedulerTickSeconds = 0.10f;

    if (!g_pNativeLightningScheduler || !g_pWeatherEffectGateByte || !self || !weatherState) {
        return;
    }

    static float s_schedulerAccum = 0.0f;
    if (!g_oThunder.active.load()) {
        s_schedulerAccum = 0.0f;
        return;
    }

    float thunder = g_oThunder.value.load();
    if (!std::isfinite(thunder) || thunder <= 0.0001f) {
        s_schedulerAccum = 0.0f;
        return;
    }
    thunder = min(1.0f, max(0.0f, thunder));
    s_schedulerAccum = min(0.5f, s_schedulerAccum + max(0.0f, dt));
    if (s_schedulerAccum < kThunderSchedulerTickSeconds) {
        return;
    }
    const float schedulerDt = s_schedulerAccum;
    s_schedulerAccum = 0.0f;

    const float rainHint = !g_noRain.load() && g_oRain.active.load() && std::isfinite(g_oRain.value.load())
        ? min(1.0f, max(0.0f, g_oRain.value.load()))
        : -1.0f;
    if (!IsReadableTickPtr(static_cast<uintptr_t>(self), g_lightningNextDelayOffset + sizeof(float)) ||
        !IsReadableTickPtr(reinterpret_cast<uintptr_t>(g_pWeatherEffectGateByte), sizeof(*g_pWeatherEffectGateByte))) {
        return;
    }

    uint8_t gate = 0;
    ComposedWeatherFields weatherFields{};
    if (!ResolveComposedWeatherFields(weatherState, weatherFields) || !weatherFields.rain) {
        return;
    }

    float savedRain = 0.0f;
    bool rainTemporarilyRaised = false;
    __try {
        gate = *g_pWeatherEffectGateByte;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    const bool gateEnabled = g_weatherEffectGateEnabledWhenZero ? gate == 0 : gate != 0;

    long long effect = 0;
    long long variation = 0;
    float elapsedBefore = 0.0f;
    float nextBefore = -1.0f;
    if (!TryReadTickValue(self, 0x78, effect) || !TryReadTickValue(self, 0x80, variation) ||
        !TryReadTickValue(self, g_lightningElapsedOffset, elapsedBefore) ||
        !TryReadTickValue(self, g_lightningNextDelayOffset, nextBefore)) {
        return;
    }
    if (!effect || !variation) {
        static DWORD64 s_lastMissingEffectLog = 0;
        const DWORD64 now = GetTickCount64();
        if (now - s_lastMissingEffectLog >= 5000) {
            s_lastMissingEffectLog = now;
            Log("[thunder] scheduler skipped, effect=%p variation=%p self=%p\n",
                reinterpret_cast<void*>(effect), reinterpret_cast<void*>(variation), reinterpret_cast<void*>(self));
        }
        return;
    }

    __try {
        float& elapsed = At<float>(self, g_lightningElapsedOffset);
        float& nextDelay = At<float>(self, g_lightningNextDelayOffset);
        const float rate = ThunderRateCurve(thunder);
        const float maxDelay = 0.85f + (1.0f - rate) * 18.0f;
        const float schedulerRain = 0.85f + rate * 0.15f;
        if (!std::isfinite(elapsed) || elapsed < 0.0f || elapsed > 120.0f) {
            elapsed = 0.0f;
        }
        elapsed = min(120.0f, elapsed + schedulerDt);
        if (std::isfinite(nextDelay) && nextDelay > maxDelay) {
            nextDelay = maxDelay;
        }
        savedRain = *weatherFields.rain;
        if (std::isfinite(savedRain) && savedRain < schedulerRain) {
            *weatherFields.rain = schedulerRain;
            rainTemporarilyRaised = true;
        }
        g_pNativeLightningScheduler(self);
        if (rainTemporarilyRaised) {
            *weatherFields.rain = savedRain;
            rainTemporarilyRaised = false;
        }
        if (std::isfinite(nextDelay) && nextDelay > maxDelay) {
            nextDelay = maxDelay;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (rainTemporarilyRaised && weatherFields.rain) {
            __try {
                *weatherFields.rain = savedRain;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        Log("[thunder] scheduler exception self=%p gate=%u\n", reinterpret_cast<void*>(self), gate);
        return;
    }

    float elapsedAfter = 0.0f;
    float nextAfter = -1.0f;
    TryReadTickValue(self, g_lightningElapsedOffset, elapsedAfter);
    TryReadTickValue(self, g_lightningNextDelayOffset, nextAfter);

    const bool spawnedStrike = elapsedBefore > 0.5f && elapsedAfter < 0.1f && nextAfter < 0.0f;
    if (spawnedStrike) {
        TryPlayThunderAudio();
    }

    static DWORD64 s_lastLog = 0;
    const DWORD64 now = GetTickCount64();
    if (now - s_lastLog >= 10000) {
        s_lastLog = now;
        Log("[thunder] amount=%.3f rain=%.3f gate=%u enabled=%d elapsed[0x%zX]=%.3f->%.3f next[0x%zX]=%.3f->%.3f effect=%p var=%p\n",
            thunder, rainHint, gate, gateEnabled ? 1 : 0,
            static_cast<size_t>(g_lightningElapsedOffset), elapsedBefore, elapsedAfter,
            static_cast<size_t>(g_lightningNextDelayOffset), nextBefore, nextAfter,
            reinterpret_cast<void*>(effect), reinterpret_cast<void*>(variation));
    }
}

static bool WeatherTickTimeWorkNeeded() {
    if (!g_timeLayoutReady.load()) {
        return false;
    }
    return !g_timeCurrentHourValid.load() ||
        g_timeCtrlActive.load() ||
        g_timeFreeze.load() ||
        g_timeApplyRequest.load() ||
        g_timeFreezeApplied.load() ||
        g_timeSetHoldTicks.load() > 0;
}

struct SnowCoverageGlobal {
    SliderOverride* overrideValue;
    uintptr_t rva;
    float defaultValue;
};

static SnowCoverageGlobal* SnowCoverageGlobals(size_t& count) {
    static SnowCoverageGlobal kGlobals[] = {
        { &g_oSnowAccumBoundaryA, 0x5F23698, -5.0f },
        { &g_oSnowAccumBoundaryB, 0x5F236E8, -20.0f },
        { &g_oSnowCoverageThreshold, 0x5F23738, -20.0f },
    };
    count = std::size(kGlobals);
    return kGlobals;
}

static bool SnowCoverageOverrideActive() {
    size_t count = 0;
    SnowCoverageGlobal* globals = SnowCoverageGlobals(count);
    for (size_t i = 0; i < count; ++i) {
        if (globals[i].overrideValue && globals[i].overrideValue->active.load()) {
            return true;
        }
    }
    return false;
}

static bool WriteGameFloatRva(uintptr_t rva, float value) {
    if (!rva || !std::isfinite(value)) {
        return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!base) {
        return false;
    }
    __try {
        *reinterpret_cast<float*>(base + rva) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void ApplySnowCoverageGlobalOverrides(bool modEnabled) {
    const bool active = modEnabled && SnowCoverageOverrideActive();
    const bool dirty = g_snowCoverageGlobalsDirty.exchange(false);
    if (!active && !dirty) {
        return;
    }

    size_t count = 0;
    SnowCoverageGlobal* globals = SnowCoverageGlobals(count);
    for (size_t i = 0; i < count; ++i) {
        const auto& desc = globals[i];
        const bool useOverride = modEnabled && desc.overrideValue && desc.overrideValue->active.load();
        const float value = useOverride ? desc.overrideValue->value.load() : desc.defaultValue;
        WriteGameFloatRva(desc.rva, value);
    }
}

static bool WeatherTickRuntimeWorkNeeded(bool resetStopNow, bool modSuspendNow, bool modEnabled, bool presetNeedsTick) {
    if (resetStopNow || modSuspendNow || presetNeedsTick) {
        return true;
    }
    if (!modEnabled) {
        return g_timeFreezeApplied.load() || g_timeSetHoldTicks.load() > 0 || g_snowCoverageGlobalsDirty.load();
    }
    if (WeatherTickTimeWorkNeeded()) {
        return true;
    }
    if (WeatherTickRegionWorkNeeded()) {
        return true;
    }
    if (g_snowCoverageGlobalsDirty.load() || SnowCoverageOverrideActive()) {
        return true;
    }
    if (g_forceClear.load() ||
        AnyCustomWeatherSliderActive() ||
        g_activeWeather == kCustomWeather ||
        SnowEffectCleanupActive() ||
        g_noRain.load() ||
        g_noSnow.load() ||
        g_noDust.load() ||
        g_oThunder.active.load() ||
        g_noWind.load()) {
        return true;
    }
    return false;
}

static bool WeatherTickShouldRunService(float dt, bool forceNow, float& outServiceDt) {
    constexpr float kServiceIntervalSeconds = 0.20f;
    static float s_accumulatedDt = 0.0f;

    float frameDt = (std::isfinite(dt) && dt > 0.0f) ? dt : (1.0f / 60.0f);
    frameDt = min(frameDt, 0.25f);
    s_accumulatedDt = min(1.0f, s_accumulatedDt + frameDt);

    if (!forceNow && s_accumulatedDt < kServiceIntervalSeconds) {
        return false;
    }

    outServiceDt = s_accumulatedDt;
    s_accumulatedDt = 0.0f;
    return true;
}

// Hooked weather tick.
void __fastcall Hooked_WeatherTick(long long self, float dt) {
    const bool resetStopNow = g_resetStopRequested.exchange(false);
    const bool modSuspendNow = g_modSuspendRequested.exchange(false);
    const bool modEnabled = g_modEnabled.load();
    const bool presetNeedsTick = Preset_NeedsWorldTick();
    const bool runtimeWorkNeeded = WeatherTickRuntimeWorkNeeded(resetStopNow, modSuspendNow, modEnabled, presetNeedsTick);
    if (!runtimeWorkNeeded) {
        g_pOriginalTick(self, dt);
        return;
    }

    float serviceDt = 0.0f;
    const bool forceServiceNow = resetStopNow || modSuspendNow || g_timeApplyRequest.load();
    if (!WeatherTickShouldRunService(dt, forceServiceNow, serviceDt)) {
        g_pOriginalTick(self, dt);
        if (modEnabled && WeatherTickTimeWorkNeeded()) {
            // Keep Progress Visual Time at its requested cadence; only the heavier weather service is throttled.
            TickTimeControl();
        }
        return;
    }

    const ResolvedEnv env = ResolveEnv();
    const bool worldReady = env.entity && env.weatherState;
    const bool regionNeedsTick = WeatherTickRegionWorkNeeded();
    if (presetNeedsTick || regionNeedsTick) {
        UpdateRegionState(env, serviceDt);
    }
    if (presetNeedsTick) {
        Preset_OnWorldTick(worldReady, serviceDt);
    }
    UpdateSnowEffectTransitionCleanup();

    if (!modEnabled) {
        g_pOriginalTick(self, dt);
        ApplySnowCoverageGlobalOverrides(false);
        if (modSuspendNow || resetStopNow) {
            StopAllWeatherEffects(self);
            g_activeWeather = -1;
        }
        SuspendTimeControl();
        return;
    }

    if (g_forceClear.load()) {
        g_pOriginalTick(self, dt);
        ApplySnowCoverageGlobalOverrides(modEnabled);
        StopWeatherEffectsByMask(self, 0x1CCu);
        TickSnowEffectCleanup(self);
        if (resetStopNow) {
            StopAllWeatherEffects(self);
        }
        TickTimeControl();
        return;
    }

    if (AnyCustomWeatherSliderActive())
        EnterCustomMode();
    else if (g_activeWeather == kCustomWeather) {
        StopAllWeatherEffects(self);
        g_activeWeather = -1;
    }

    g_pOriginalTick(self, dt);
    ApplySnowCoverageGlobalOverrides(modEnabled);
    if (g_activeWeather == kCustomWeather) {
        TickWeatherState(self, serviceDt);
    }
    TickSnowEffectCleanup(self);
    const uint32_t suppressedWeatherMask = ComputeSuppressedWeatherEffectMask();
    if (suppressedWeatherMask) {
        StopWeatherEffectsByMask(self, suppressedWeatherMask & ~0x013u);
    }
    TickNativeLightningBridge(self, serviceDt, env.weatherState);
    if (resetStopNow) {
        StopAllWeatherEffects(self);
    }
    if (g_noWind.load()) {
        ApplyNoWindPolicy(self, env);
    }
    TickTimeControl();
}


