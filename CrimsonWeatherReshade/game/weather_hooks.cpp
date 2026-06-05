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

static float DustSliderToNative(float dust) {
    float d = max(0.0f, dust);
    return d * 15.0f;
}

static float DustSliderToFogB(float dust) {
    return 0.28f + max(0.0f, dust) * 0.14f;
}

static float DustSliderToThreshold(float dust) {
    return -15.0f - max(0.0f, dust) * 7.5f;
}

static float DustSliderToStorm(float dust) {
    return 40.0f + DustSliderToNative(dust);
}

static float DustWindControlMultiplier() {
    float mul = g_windMul.load();
    if (!std::isfinite(mul)) {
        mul = 1.0f;
    }
    return min(3.0f, max(0.0f, mul));
}

static float DustSliderToWindScale(float dust) {
    return max(0.0f, dust) * 0.60f * DustWindControlMultiplier();
}

static bool DustForcesCalmWind() {
    return g_oDust.active.load() && DustWindControlMultiplier() <= 0.001f;
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
        if (!vt || !IsReadableTickPtr(reinterpret_cast<uintptr_t>(vt), 0x48)) {
            return r;
        }

        auto getEntity = reinterpret_cast<long long(__fastcall*)(void*)>(vt[0x40 / 8]);
        if (!getEntity || !IsReadableTickPtr(reinterpret_cast<uintptr_t>(getEntity), 16)) {
            return r;
        }

        r.entity = getEntity(envMgr);
        if (!r.entity) {
            return r;
        }

        if (!IsReadableTickPtr(static_cast<uintptr_t>(r.entity + 0xEE0), sizeof(long long)) ||
            !IsReadableTickPtr(static_cast<uintptr_t>(r.entity + 0xEE8), sizeof(long long))) {
            return r;
        }

        r.weatherState = *reinterpret_cast<long long*>(r.entity + 0xEE0);
        r.particleMgr = *reinterpret_cast<long long*>(r.entity + 0xEE8);
        if (!r.weatherState || !IsReadableTickPtr(static_cast<uintptr_t>(r.weatherState), 0x58)) {
            return r;
        }
        if (r.particleMgr && !IsReadableTickPtr(static_cast<uintptr_t>(r.particleMgr), 0x20)) {
            r.particleMgr = 0;
        }

        long long result = *reinterpret_cast<long long*>(r.weatherState + 0x50);
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
    if (g_oDust.active.load()) {
        ResolvedEnv dustEnv = ResolveTickEnvCurrentBuild();
        if (dustEnv.valid) {
            return dustEnv;
        }
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

// Intensity hooks feed slider overrides into the engine weather blend inputs.
__m128 __fastcall Hooked_GetRainIntensity(long long ws) {
    if (!g_modEnabled.load()) {
        return g_pOrigGetRainIntensity ? g_pOrigGetRainIntensity(ws) : PackScalar(0.0f);
    }
    if (g_forceClear.load()) return PackScalar(0.0f);
    const float thunderRainBias = g_thunderSchedulerRainBias.load();
    if (std::isfinite(thunderRainBias) && thunderRainBias > 0.0001f) {
        const float baseRain = (!g_noRain.load() && g_oRain.active.load()) ? g_oRain.value.load() : 0.0f;
        return PackScalar(max(baseRain, thunderRainBias));
    }
    if (g_noRain.load()) return PackScalar(0.0f);
    if (g_oRain.active.load())
        return PackScalar(g_oRain.value.load());
    float baseRain = g_pOrigGetRainIntensity ? ExtractScalar(g_pOrigGetRainIntensity(ws)) : 0.0f;
    if (baseRain > 0.01f) {
        static ULONGLONG s_lastRainBaseLog = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_lastRainBaseLog > 1500) {
            s_lastRainBaseLog = now;
            Log("[rain] base weather rain=%.3f (override inactive)\n", baseRain);
        }
    }
    return PackScalar(baseRain);
}

__m128 __fastcall Hooked_GetSnowIntensity(long long ws) {
    if (!g_modEnabled.load()) {
        return g_pOrigGetSnowIntensity ? g_pOrigGetSnowIntensity(ws) : PackScalar(0.0f);
    }
    if (g_forceClear.load() || g_noSnow.load()) return PackScalar(0.0f);
    if (g_oSnow.active.load())
        return PackScalar(g_oSnow.value.load());
    return g_pOrigGetSnowIntensity ? g_pOrigGetSnowIntensity(ws) : PackScalar(0.0f);
}

__m128 __fastcall Hooked_GetDustIntensity(long long ws) {
    if (!g_modEnabled.load()) {
        return g_pOrigGetDustIntensity ? g_pOrigGetDustIntensity(ws) : PackScalar(0.0f);
    }
    if (g_noDust.load() || g_noWind.load())
        return PackScalar(0.0f);
    float mul = g_windMul.load();
    if (mul < 0.0f) mul = 0.0f;
    if (mul > 15.0f) mul = 15.0f;
    if (g_oDust.active.load()) {
        return PackScalar(DustSliderToNative(g_oDust.value.load()) * mul);
    }
    float v = g_pOrigGetDustIntensity ? ExtractScalar(g_pOrigGetDustIntensity(ws)) : 0.0f;
    return PackScalar(v * mul);
}

static constexpr uint32_t kFogReceiverOverrideMask = 0x1F;
static constexpr float kFogOverdriveNormAt100 = 2.4f;
static constexpr float kFogDenseMax = 500.0f;
static inline void ApplyAuthoritativeFogProfile(float fogValue,
                                                float& v0, float& v1, float& v2, float& v3, float& v4) {
    float fog = max(0.0f, fogValue);       
    float fogN = (fog * 0.01f) * kFogOverdriveNormAt100; 
    if (fogN <= 0.01f) {
        v0 = 0.0f;
        v1 = 0.0f;
        v2 = 0.0f;
        v3 = 0.0f;
        v4 = 2.0f;
        return;
    }

    float dense = min(kFogDenseMax, (fogN * fogN) * (6.0f + 14.0f * fogN));
    v0 = dense;
    v1 = dense;
    v2 = dense;
    v3 = dense;
    v4 = max(0.01f, 2.0f * (1.0f - 0.995f * fogN));
}

struct FogProfile {
    float v[5];
};

static bool BuildForcedFogProfile(FogProfile& out) {
    const bool forceClear = g_forceClear.load();
    const bool noFog = g_noFog.load();
    if (forceClear || noFog) {
        out.v[0] = 0.0f;
        out.v[1] = 0.0f;
        out.v[2] = 0.0f;
        out.v[3] = 0.0f;
        out.v[4] = 2.0f;
        return true;
    }
    if (!g_oFog.active.load()) {
        return false;
    }
    ApplyAuthoritativeFogProfile(max(0.0f, g_oFog.value.load()), out.v[0], out.v[1], out.v[2], out.v[3], out.v[4]);
    return true;
}

static void StoreForcedFogProfile(const FogProfile& profile) {
    for (int i = 0; i < 5; ++i) {
        g_forcedFogSet[i].store(profile.v[i]);
    }
}

static bool FogProfileNearlyEqual(const FogProfile& a, const FogProfile& b) {
    for (int i = 0; i < 5; ++i) {
        if (fabsf(a.v[i] - b.v[i]) > 0.0005f) {
            return false;
        }
    }
    return true;
}

void __fastcall Hooked_AtmosFogBlend(long long ctx, long long outParams) {
    if (g_pOrigAtmosFogBlend) g_pOrigAtmosFogBlend(ctx, outParams);
    if (!g_modEnabled.load()) return;
    if (!outParams) return;

    FogProfile profile{};
    if (BuildForcedFogProfile(profile)) {
        __try {
            float* p = reinterpret_cast<float*>(outParams + 0x10);
            for (int i = 0; i < 5; ++i) {
                p[i] = profile.v[i];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
}

static bool AnyPackedCelestialOverrideActive() {
    return g_oSunSize.active.load() || g_oMoonSize.active.load() || g_oExpNightSkyRot.active.load();
}

static float ResolveFogSetValue(int idx, float incoming) {
    if (!g_modEnabled.load()) return incoming;
    if (idx < 0 || idx >= 5) return incoming;
    if ((kFogReceiverOverrideMask & (1u << idx)) == 0) return incoming;
    if (g_forceClear.load() || g_noFog.load()) {
        static constexpr float kClearFogProfile[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 2.0f };
        return kClearFogProfile[idx];
    }
    if (!g_oFog.active.load()) return incoming;
    float forced = g_forcedFogSet[idx].load();
    return std::isfinite(forced) ? forced : incoming;
}

template <int Index>
static void __fastcall Hooked_FogSetImpl(long long* receiver, float value) {
    float outV = ResolveFogSetValue(Index, value);
    auto fn = g_pOrigFogSet[Index];
    if (fn) fn(receiver, outV);
}

void __fastcall Hooked_FogSet0(long long* receiver, float value) {
    Hooked_FogSetImpl<0>(receiver, value);
}
void __fastcall Hooked_FogSet1(long long* receiver, float value) {
    Hooked_FogSetImpl<1>(receiver, value);
}
void __fastcall Hooked_FogSet2(long long* receiver, float value) {
    Hooked_FogSetImpl<2>(receiver, value);
}
void __fastcall Hooked_FogSet3(long long* receiver, float value) {
    Hooked_FogSetImpl<3>(receiver, value);
}
void __fastcall Hooked_FogSet4(long long* receiver, float value) {
    Hooked_FogSetImpl<4>(receiver, value);
}

static void TryInstallFogSetHooks(uintptr_t* rVt) {
    if (!rVt) return;
    bool expected = false;
    if (!g_fogSetHooksAttempted.compare_exchange_strong(expected, true)) return;

    struct HookDesc { int idx; int vtOff; void* detour; const char* name; };
    const HookDesc kHooks[5] = {
        {0, 0x08, (void*)&Hooked_FogSet0, "FogSet0"},
        {1, 0x10, (void*)&Hooked_FogSet1, "FogSet1"},
        {2, 0x18, (void*)&Hooked_FogSet2, "FogSet2"},
        {3, 0x28, (void*)&Hooked_FogSet3, "FogSet3"},
        {4, 0x20, (void*)&Hooked_FogSet4, "FogSet4"},
    };

    bool anyInstalled = false;
    for (const auto& h : kHooks) {
        uintptr_t addr = rVt[h.vtOff / 8];
        g_addrFogSet[h.idx] = addr;
        if (!addr) {
            continue;
        }

        bool dup = false;
        for (int i = 0; i < h.idx; ++i) {
            if (g_addrFogSet[i] == addr) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        bool ok = InstallHook((void*)addr, h.detour, (void**)&g_pOrigFogSet[h.idx], h.name, false);
        if (ok && g_pOrigFogSet[h.idx]) anyInstalled = true;
    }

    g_fogSetHooksInstalled.store(anyInstalled);
}

static bool ResolveFogReceiverFromFrame(long long* self, long long*& receiver, FogReceiverSet_fn* setters) {
    receiver = nullptr;
    if (setters) {
        for (int i = 0; i < 5; ++i) {
            setters[i] = nullptr;
        }
    }
    if (!self) return false;
    __try {
        long long provider = *reinterpret_cast<long long*>(reinterpret_cast<uint8_t*>(self) + 0x48);
        if (!provider) return false;
        auto pVt = *reinterpret_cast<uintptr_t**>(provider);
        if (!pVt) return false;
        auto getReceiver = reinterpret_cast<FogReceiverGetter_fn>(pVt[0x190 / 8]);
        if (!getReceiver) return false;
        receiver = getReceiver(provider);
        if (!receiver) return false;
        auto rVt = *reinterpret_cast<uintptr_t**>(receiver);
        if (!rVt) return false;

        TryInstallFogSetHooks(rVt);
        if (setters) {
            setters[0] = reinterpret_cast<FogReceiverSet_fn>(rVt[0x08 / 8]);
            setters[1] = reinterpret_cast<FogReceiverSet_fn>(rVt[0x10 / 8]);
            setters[2] = reinterpret_cast<FogReceiverSet_fn>(rVt[0x18 / 8]);
            setters[3] = reinterpret_cast<FogReceiverSet_fn>(rVt[0x28 / 8]);
            setters[4] = reinterpret_cast<FogReceiverSet_fn>(rVt[0x20 / 8]);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        receiver = nullptr;
        return false;
    }
}

static void ForceApplyFogFromFrame(long long* self) {
    FogProfile profile{};
    if (!BuildForcedFogProfile(profile)) return;
    StoreForcedFogProfile(profile);

    static FogProfile s_lastProfile{ { NAN, NAN, NAN, NAN, NAN } };
    static uintptr_t s_lastReceiver = 0;
    static DWORD64 s_lastRefreshMs = 0;
    const DWORD64 now = GetTickCount64();
    const bool profileChanged = !FogProfileNearlyEqual(profile, s_lastProfile);
    const bool refreshDue = now - s_lastRefreshMs >= 250;
    if (!profileChanged && !refreshDue) {
        return;
    }

    long long* receiver = nullptr;
    FogReceiverSet_fn setters[5] = {};
    if (!ResolveFogReceiverFromFrame(self, receiver, setters)) {
        return;
    }

    const uintptr_t receiverAddr = reinterpret_cast<uintptr_t>(receiver);
    const bool receiverChanged = receiverAddr != s_lastReceiver;
    if (!profileChanged && !receiverChanged && !refreshDue) {
        return;
    }

    __try {
        if ((kFogReceiverOverrideMask & (1u << 0)) != 0 && setters[0]) setters[0](receiver, profile.v[0]);
        if ((kFogReceiverOverrideMask & (1u << 1)) != 0 && setters[1]) setters[1](receiver, profile.v[1]);
        if ((kFogReceiverOverrideMask & (1u << 2)) != 0 && setters[2]) setters[2](receiver, profile.v[2]);
        if ((kFogReceiverOverrideMask & (1u << 3)) != 0 && setters[3]) setters[3](receiver, profile.v[3]);
        if ((kFogReceiverOverrideMask & (1u << 4)) != 0 && setters[4]) setters[4](receiver, profile.v[4]);
        s_lastProfile = profile;
        s_lastReceiver = receiverAddr;
        s_lastRefreshMs = now;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        s_lastReceiver = 0;
    }
}

static bool WeatherFrameFogWorkNeeded() {
    return g_forceClear.load() || g_noFog.load() || g_oFog.active.load();
}

static void ResetFogBlendWeightsForNativeRefresh(long long* self) {
    if (!self) {
        return;
    }
    __try {
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0x98) = 0.0f;
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0x9C) = 0.0f;
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0xA0) = 0.0f;
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0xA4) = 0.0f;
        *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0xA8) = 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static float ThunderRateCurve(float thunder) {
    thunder = min(1.0f, max(0.0f, thunder));
    return powf(thunder, 0.55f);
}

static constexpr float kCloudGeometryMax = 64.0f;

struct CloudGeometry {
    float top;
    float thick;
    float base;
    float shapeA;
    float shapeC;
};

static void ClampCloudGeometry(CloudGeometry& g) {
    g.top = max(0.0f, g.top);
    g.thick = max(0.0f, g.thick);
    g.base = max(0.0f, g.base);
    g.shapeA = max(0.0f, g.shapeA);
    g.shapeC = max(0.0f, g.shapeC);
}

static bool IsReasonableCloudGeometry(const CloudGeometry& g) {
    return std::isfinite(g.top) && std::isfinite(g.thick) && std::isfinite(g.base) &&
        std::isfinite(g.shapeA) && std::isfinite(g.shapeC) &&
        g.top <= kCloudGeometryMax && g.thick <= kCloudGeometryMax &&
        g.base <= kCloudGeometryMax && g.shapeA <= kCloudGeometryMax &&
        g.shapeC <= kCloudGeometryMax;
}

static bool ReadCloudGeometry(long long cloudNode, CloudGeometry& out) {
    if (!cloudNode) return false;
    const float top = At<float>(cloudNode, CN::CLOUD_TOP);
    const float thick = At<float>(cloudNode, CN::CLOUD_THICK);
    const float base = At<float>(cloudNode, CN::CLOUD_BASE);
    const float sA = At<float>(cloudNode, CN::CLOUD_SHAPE_A);
    const float sC = At<float>(cloudNode, CN::CLOUD_SHAPE_C);
    if (!std::isfinite(top) || !std::isfinite(thick) || !std::isfinite(base) ||
        !std::isfinite(sA) || !std::isfinite(sC)) {
        return false;
    }
    out = { top, thick, base, sA, sC };
    ClampCloudGeometry(out);
    return IsReasonableCloudGeometry(out);
}

static bool LoadStoredCloudBase(CloudGeometry& out) {
    if (!g_cloudBaseValid.load()) return false;
    out.top = max(0.0f, g_cloudBaseTop.load());
    out.thick = max(0.0f, g_cloudBaseThick.load());
    out.base = max(0.0f, g_cloudBaseBase.load());
    out.shapeA = max(0.0f, g_cloudBaseShapeA.load());
    out.shapeC = max(0.0f, g_cloudBaseShapeC.load());
    return IsReasonableCloudGeometry(out);
}

static bool BuildEffectiveCloudGeometry(long long cloudNode, CloudGeometry& base,
                                        CloudGeometry& effective) {
    if (!LoadStoredCloudBase(base) && !ReadCloudGeometry(cloudNode, base)) {
        return false;
    }
    effective = base;
    return true;
}

static void ApplyCloudShapeMultipliers(const CloudGeometry& source, float mulX, float mulZ,
                                       CloudGeometry& out) {
    out = source;
    const float center = 0.5f * (source.top + source.base);
    const float halfSpan = max(0.0f, 0.5f * (source.top - source.base)) * mulX;
    out.top = max(0.0f, center + halfSpan);
    out.base = max(0.0f, center - halfSpan);
    out.shapeA = max(0.0f, source.shapeA * mulZ);
    out.shapeC = max(0.0f, source.shapeC * mulZ);
}

static void CaptureCloudBaseline(const ResolvedEnv& env) {
    if (!env.valid) return;
    const bool hadBaseline = g_cloudBaseValid.load();
    const bool cloudShapeOverrideActive = g_oCloudSpdY.active.load();
    CloudGeometry live{};
    if (ReadCloudGeometry(env.cloudNode, live)) {
        if (!(cloudShapeOverrideActive && hadBaseline)) {
            g_cloudBaseTop.store(live.top);
            g_cloudBaseThick.store(live.thick);
            g_cloudBaseBase.store(live.base);
            g_cloudBaseShapeA.store(live.shapeA);
            g_cloudBaseShapeC.store(live.shapeC);
            g_cloudBaseValid.store(true);
        }

    }

}

static void ApplyCloudOverrides(const ResolvedEnv& env) {
    if (!env.valid) return;
    const bool cloudShapeActive = g_oCloudSpdY.active.load();
    if (!cloudShapeActive) return;

    const float mulX = 1.0f;
    const float mulZ = cloudShapeActive ? min(10.0f, max(0.0f, g_oCloudSpdY.get(1.0f))) : 1.0f;

    if (cloudShapeActive && env.cloudNode) {
        CloudGeometry base{};
        CloudGeometry effective{};
        if (BuildEffectiveCloudGeometry(env.cloudNode, base, effective)) {

            CloudGeometry rendered = effective;
            ApplyCloudShapeMultipliers(effective, mulX, mulZ, rendered);

            At<float>(env.cloudNode, CN::CLOUD_TOP) = rendered.top;
            At<float>(env.cloudNode, CN::CLOUD_BASE) = rendered.base;
            At<float>(env.cloudNode, CN::CLOUD_SHAPE_A) = rendered.shapeA;
            At<float>(env.cloudNode, CN::CLOUD_SHAPE_C) = rendered.shapeC;
        }
    }
}

struct AtmosphereCloudPack {
    float baseScale;
    float visibleRange;
    float density;
    float contrast;
    float alpha;
    float scroll;
    float altitude;
    float thickness;
    float nearPlane;
};

static bool ReadAtmosphereCloudPack(const float* packedOut, AtmosphereCloudPack& out) {
    if (!packedOut) return false;
    out.baseScale = packedOut[0x04];
    out.visibleRange = packedOut[0x0A];
    out.density = packedOut[0x2C];
    out.contrast = packedOut[0x2D];
    out.alpha = packedOut[0x2F];
    out.scroll = packedOut[0x30];
    out.altitude = packedOut[0x31];
    out.thickness = packedOut[0x32];
    out.nearPlane = packedOut[0x33];
    return std::isfinite(out.baseScale) && std::isfinite(out.visibleRange) &&
           std::isfinite(out.density) && std::isfinite(out.contrast) &&
           std::isfinite(out.alpha) && std::isfinite(out.scroll) &&
           std::isfinite(out.altitude) && std::isfinite(out.thickness) &&
           std::isfinite(out.nearPlane);
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

static constexpr int kPackedSunSize = 0x02;
static constexpr int kPackedSunSizeCos = 0x03;
static constexpr int kPackedSunDirection = 0x04;
static constexpr int kPackedMoonSize = 0x07;
static constexpr int kPackedMoonSizeCos = 0x08;
static constexpr int kPackedMoonDirection = 0x09;
static constexpr int kPackedEarthAxisTilt = 0x0A;
static constexpr int kPackedLatitude = 0x0B;
static constexpr float kPackedAngleToRad = 0.01745329251994329577f;
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

static float FloatFromBits(unsigned int bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

static unsigned int PackRgbBits(float r, float g, float b) {
    const auto toByte = [](float value) -> unsigned int {
        return static_cast<unsigned int>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return (toByte(r) << 16) | (toByte(g) << 8) | toByte(b);
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

static void CapturePackedCelestialBase(const float* packedOut) {
    if (!packedOut) return;
    if (!std::isfinite(packedOut[kPackedSunSize]) ||
        !std::isfinite(packedOut[kPackedSunDirection]) ||
        !std::isfinite(packedOut[kPackedMoonSize]) ||
        !std::isfinite(packedOut[kPackedMoonDirection])) {
        return;
    }

    if (!g_oSunSize.active.load()) {
        g_atmoBaseSunSize.store(packedOut[kPackedSunSize]);
    }
    if (!g_oMoonSize.active.load()) {
        g_atmoBaseMoonSize.store(packedOut[kPackedMoonSize]);
    }
    if (!g_oExpNightSkyRot.active.load() && std::isfinite(packedOut[kPackedEarthAxisTilt])) {
        g_windPackBase0A.store(packedOut[kPackedEarthAxisTilt]);
        g_windPackBase0AValid.store(true);
    }
    if (!g_oExpNightSkyRot.active.load() && std::isfinite(packedOut[kPackedLatitude])) {
        g_windPackBase0B.store(packedOut[kPackedLatitude]);
        g_windPackBase0BValid.store(true);
    }

    if (!g_atmoCelestialBaseValid.load()) {
        g_atmoCelestialBaseValid.store(true);
    }
}

static void ApplyPackedCelestialOverrides(float* packedOut) {
    if (!packedOut) return;
    CapturePackedCelestialBase(packedOut);

    if (g_oSunLightIntensity.active.load() && std::isfinite(g_oSunLightIntensity.value.load())) {
        packedOut[0x00] = ClampFloat(g_oSunLightIntensity.value.load(), 0.0f, 100.0f);
    }
    if (g_oMoonLightIntensity.active.load() && std::isfinite(g_oMoonLightIntensity.value.load())) {
        packedOut[0x05] = ClampFloat(g_oMoonLightIntensity.value.load(), 0.0f, 100.0f);
    }
    if (g_oRayleighScatteringColor.active.load()) {
        packedOut[0x0F] = FloatFromBits(PackRgbBits(
            g_oRayleighScatteringColor.r.load(),
            g_oRayleighScatteringColor.g.load(),
            g_oRayleighScatteringColor.b.load()));
    }
    if (g_oRayleighHeight.active.load() && std::isfinite(g_oRayleighHeight.value.load())) {
        packedOut[0x0E] = ClampFloat(g_oRayleighHeight.value.load(), 1.0f, 200000.0f);
    }
    if (g_oOzoneRatio.active.load() && std::isfinite(g_oOzoneRatio.value.load())) {
        packedOut[0x14] = ClampFloat(g_oOzoneRatio.value.load(), 0.0f, 100.0f);
    }

    if (!AnyPackedCelestialOverrideActive() || !g_atmoCelestialBaseValid.load()) return;

    if (g_oSunSize.active.load()) {
        const float size = ClampFloat(g_oSunSize.value.load(), 0.01f, 10.0f);
        packedOut[kPackedSunSize] = size;
        packedOut[kPackedSunSizeCos] = cosf(size * kPackedAngleToRad);
    }
    if (g_oMoonSize.active.load()) {
        const float size = ClampFloat(g_oMoonSize.value.load(), 0.001f, 100.0f);
        packedOut[kPackedMoonSize] = size;
        packedOut[kPackedMoonSizeCos] = cosf(size * kPackedAngleToRad);
    }
    if (g_oExpNightSkyRot.active.load() && g_windPackBase0BValid.load()) {
        const float pitch = ClampFloat(g_oExpNightSkyRot.value.load(), -89.0f, 89.0f);
        packedOut[kPackedEarthAxisTilt] = pitch - 90.0f + g_windPackBase0B.load();
    }

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

static bool ApplySceneCelestialOverrides(float* scene) {
    if (!scene) return false;
    CaptureSceneCelestialBase(scene);
    const bool sceneOverrideActive = g_oSunDirX.active.load() || g_oSunDirY.active.load() ||
                                     g_oMoonDirX.active.load() || g_oMoonDirY.active.load() ||
                                     g_oMoonRoll.active.load() || g_oNightSkyYaw.active.load();
    if (!sceneOverrideActive || !g_sceneCelestialBaseValid.load()) return false;

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
    if (!g_modEnabled.load() || !self) return result;

    __try {
        auto* sceneSource = *reinterpret_cast<float**>(self + 0x428);
        if (sceneSource) {
            ApplySceneCelestialOverrides(sceneSource);
        }

        auto* sceneOwner = *reinterpret_cast<uint8_t**>(self + 0x430);
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

void __fastcall Hooked_WindPack(long long* windNodePtr, float* packedOut) {
    const bool modEnabled = g_modEnabled.load();
    if (g_pOrigWindPack) g_pOrigWindPack(windNodePtr, packedOut);
    if (!packedOut) return;
    if (!modEnabled) return;

    ApplyPackedCelestialOverrides(packedOut);

    AtmosphereCloudPack nativeCloudPack{};
    const bool nativeCloudValid = ReadAtmosphereCloudPack(packedOut, nativeCloudPack);
    float v23 = packedOut[0x23], v24 = packedOut[0x24];
    float v2F = packedOut[0x2F], v30 = packedOut[0x30];
    bool finiteAll = std::isfinite(v23) && std::isfinite(v24) &&
                     std::isfinite(v2F) && std::isfinite(v30);
    if (finiteAll && !g_windPackBaseValid.load()) {
        g_windPackBase23.store(v23);
        g_windPackBase24.store(v24);
        g_windPackBase2F.store(v2F);
        g_windPackBase30.store(v30);
        g_windPackBaseValid.store(true);
    }
    if (nativeCloudValid) {
        if (!g_oHighClouds.active.load() && std::isfinite(nativeCloudPack.scroll)) {
            g_windPackBase30.store(nativeCloudPack.scroll);
        }
        if (!g_oAtmoAlpha.active.load() && std::isfinite(nativeCloudPack.alpha)) {
            g_windPackBase2F.store(nativeCloudPack.alpha);
        }
        if (!g_oCloudVariation.active.load() && std::isfinite(nativeCloudPack.thickness)) {
            g_windPackBase32.store(nativeCloudPack.thickness);
            g_windPackBase32Valid.store(true);
        }
    }
    if (!g_oExpCloud2C.active.load() && std::isfinite(packedOut[0x2C])) {
        g_windPackBase2C.store(packedOut[0x2C]);
        g_windPackBase2CValid.store(true);
    }
    if (!g_oExpCloud2D.active.load() && std::isfinite(packedOut[0x2D])) {
        g_windPackBase2D.store(packedOut[0x2D]);
        g_windPackBase2DValid.store(true);
    }
    if (!g_oNativeFog.active.load() && !g_oMieAerosolDensity.active.load() && std::isfinite(packedOut[0x11])) {
        g_windPackBase11.store(packedOut[0x11]);
        g_windPackBase11Valid.store(true);
    }
    if (!g_oNativeFog.active.load() && std::isfinite(packedOut[0x17])) {
        g_windPackBase17.store(packedOut[0x17]);
        g_windPackBase17Valid.store(true);
    }
    if (std::isfinite(packedOut[0x1B])) {
        g_windPackBase1B.store(packedOut[0x1B]);
        g_windPackBase1BValid.store(true);
    }
    if (!g_oSunLightIntensity.active.load() && std::isfinite(packedOut[0x00])) {
        g_windPackBase00.store(packedOut[0x00]);
        g_windPackBase00Valid.store(true);
    }
    if (!g_oMoonLightIntensity.active.load() && std::isfinite(packedOut[0x05])) {
        g_windPackBase05.store(packedOut[0x05]);
        g_windPackBase05Valid.store(true);
    }
    if (!g_oRayleighScatteringColor.active.load()) {
        g_windPackBase0FBits.store(FloatBits(packedOut[0x0F]));
        g_windPackBase0FValid.store(true);
    }
    if (!g_oRayleighHeight.active.load() && std::isfinite(packedOut[0x0E])) {
        g_windPackBase0E.store(packedOut[0x0E]);
        g_windPackBase0EValid.store(true);
    }
    if (!g_oOzoneRatio.active.load() && std::isfinite(packedOut[0x14])) {
        g_windPackBase14.store(packedOut[0x14]);
        g_windPackBase14Valid.store(true);
    }
    if (!g_oMieScaleHeight.active.load() && std::isfinite(packedOut[0x10])) {
        g_windPackBase10.store(packedOut[0x10]);
        g_windPackBase10Valid.store(true);
    }
    if (!g_oMieAerosolAbsorption.active.load() && std::isfinite(packedOut[0x12])) {
        g_windPackBase12.store(packedOut[0x12]);
        g_windPackBase12Valid.store(true);
    }
    if (!g_oHeightFogBaseline.active.load() && std::isfinite(packedOut[0x18])) {
        g_windPackBase18.store(packedOut[0x18]);
        g_windPackBase18Valid.store(true);
    }
    if (!g_oHeightFogFalloff.active.load() && std::isfinite(packedOut[0x19])) {
        g_windPackBase19.store(packedOut[0x19]);
        g_windPackBase19Valid.store(true);
    }
    if (!g_oCloudAlpha.active.load() && std::isfinite(packedOut[0x1E])) {
        g_windPackBase1E.store(packedOut[0x1E]);
        g_windPackBase1EValid.store(true);
    }
    if (!g_oCloudFlow.active.load() && std::isfinite(packedOut[0x1F])) {
        g_windPackBase1F.store(packedOut[0x1F]);
        g_windPackBase1FValid.store(true);
    }
    if (!g_oCloudScatteringCoefficient.active.load() && std::isfinite(packedOut[0x20])) {
        g_windPackBase20.store(packedOut[0x20]);
        g_windPackBase20Valid.store(true);
    }
    if (!g_oCloudPhaseFront.active.load() && std::isfinite(packedOut[0x21])) {
        g_windPackBase21.store(packedOut[0x21]);
        g_windPackBase21Valid.store(true);
    }
    if (!g_oCloudVisibleRange.active.load() && std::isfinite(packedOut[0x25])) {
        g_windPackBase25.store(packedOut[0x25]);
        g_windPackBase25Valid.store(true);
    }
    if (!g_oCloudFadeRange.active.load() && std::isfinite(packedOut[0x27])) {
        g_windPackBase27.store(packedOut[0x27]);
        g_windPackBase27Valid.store(true);
    }
    if (!g_oCloudDetailRatio.active.load() && std::isfinite(packedOut[0x28])) {
        g_windPackBase28.store(packedOut[0x28]);
        g_windPackBase28Valid.store(true);
    }
    if (!g_oVolumeFogScatterColor.active.load() &&
        std::isfinite(packedOut[0x34]) && std::isfinite(packedOut[0x35]) &&
        std::isfinite(packedOut[0x36]) && std::isfinite(packedOut[0x37])) {
        g_windPackBase34.store(packedOut[0x34]);
        g_windPackBase35.store(packedOut[0x35]);
        g_windPackBase36.store(packedOut[0x36]);
        g_windPackBase37.store(packedOut[0x37]);
        g_windPackBaseVolumeFogColorValid.store(true);
    }
    if (!g_oMieScatterColor.active.load() &&
        std::isfinite(packedOut[0x38]) && std::isfinite(packedOut[0x39]) &&
        std::isfinite(packedOut[0x3A]) && std::isfinite(packedOut[0x3B])) {
        g_windPackBase38.store(packedOut[0x38]);
        g_windPackBase39.store(packedOut[0x39]);
        g_windPackBase3A.store(packedOut[0x3A]);
        g_windPackBase3B.store(packedOut[0x3B]);
        g_windPackBaseMieScatterColorValid.store(true);
    }

    const bool forceClear = g_forceClear.load();
    const bool noFog = g_noFog.load();
    if (forceClear) {
        packedOut[0x1B] = 0.0f;
        packedOut[0x11] = 0.0f;
        packedOut[0x17] = 0.0f;
        return;
    }
    if (noFog) {
        packedOut[0x11] = 0.0f;
        packedOut[0x17] = 0.0f;
    }

    const bool cloudActive = g_oCloudSpdX.active.load() || g_oCloudSpdY.active.load();
    if (cloudActive && g_windPackBaseValid.load()) {
        float mulX = CloudHeightUiToMultiplier(g_oCloudSpdX.get(1.0f));
        float mulZ = min(10.0f, max(0.0f, g_oCloudSpdY.get(1.0f)));

        packedOut[0x23] = g_windPackBase23.load() * mulX;
        packedOut[0x24] = g_windPackBase24.load() * mulZ;
    }

    if (g_oCloudAmount.active.load() && g_windPackBase1BValid.load()) {
        const float mul = CloudAmountUiToMultiplier(g_oCloudAmount.get(1.0f));
        packedOut[0x1B] = min(3.0f, max(0.0f, g_windPackBase1B.load() * mul));
    }

    if (g_oHighClouds.active.load() && g_windPackBaseValid.load()) {
        const float midCloudsMul = min(15.0f, max(0.0f, g_oHighClouds.get(1.0f)));
        packedOut[0x30] = g_windPackBase30.load() * midCloudsMul;
    }

    if (g_oAtmoAlpha.active.load() && g_windPackBaseValid.load()) {
        const float highCloudsMul = min(15.0f, max(0.0f, g_oAtmoAlpha.get(1.0f)));
        packedOut[0x2F] = g_windPackBase2F.load() * highCloudsMul;
    }

    const bool exp2CActive = g_oExpCloud2C.active.load() && g_windPackBase2CValid.load();
    const bool exp2DActive = g_oExpCloud2D.active.load() && g_windPackBase2DValid.load();
    if (exp2CActive) {
        const float mul = min(15.0f, max(0.0f, g_oExpCloud2C.get(1.0f)));
        packedOut[0x2C] = g_windPackBase2C.load() * mul;
    }
    if (exp2DActive) {
        const float mul = min(15.0f, max(0.0f, g_oExpCloud2D.get(1.0f)));
        packedOut[0x2D] = g_windPackBase2D.load() * mul;
    }

    if (g_oCloudVariation.active.load() && g_windPackBase32Valid.load()) {
        const float mul = min(15.0f, max(0.0f, g_oCloudVariation.get(1.0f)));
        packedOut[0x32] = g_windPackBase32.load() * mul;
    }

    if (!noFog && g_oNativeFog.active.load()) {
        const float fogBoost = min(15.0f, max(0.0f, g_oNativeFog.value.load()));
        const float fogMul = 1.0f + fogBoost;
        if (g_windPackBase11Valid.load()) {
            packedOut[0x11] = g_windPackBase11.load() * fogMul;
        }
        if (g_windPackBase17Valid.load()) {
            packedOut[0x17] = g_windPackBase17.load() * fogMul;
        }
    }

    if (!noFog) {
        if (g_oMieScaleHeight.active.load()) {
            packedOut[0x10] = ClampFloat(g_oMieScaleHeight.value.load(), 1.0f, 200000.0f);
        }
        if (g_oMieAerosolDensity.active.load()) {
            packedOut[0x11] = ClampFloat(g_oMieAerosolDensity.value.load(), 0.0f, 100.0f);
        }
        if (g_oMieAerosolAbsorption.active.load()) {
            packedOut[0x12] = ClampFloat(g_oMieAerosolAbsorption.value.load(), 0.0f, 100.0f);
        }
        if (g_oHeightFogBaseline.active.load()) {
            packedOut[0x18] = ClampFloat(g_oHeightFogBaseline.value.load(), -50000.0f, 50000.0f);
        }
        if (g_oHeightFogFalloff.active.load()) {
            packedOut[0x19] = ClampFloat(g_oHeightFogFalloff.value.load(), 0.0f, 100.0f);
        }
        if (g_oVolumeFogScatterColor.active.load()) {
            packedOut[0x34] = g_oVolumeFogScatterColor.r.load();
            packedOut[0x35] = g_oVolumeFogScatterColor.g.load();
            packedOut[0x36] = g_oVolumeFogScatterColor.b.load();
            packedOut[0x37] = g_oVolumeFogScatterColor.a.load();
        }
        if (g_oMieScatterColor.active.load()) {
            packedOut[0x38] = g_oMieScatterColor.r.load();
            packedOut[0x39] = g_oMieScatterColor.g.load();
            packedOut[0x3A] = g_oMieScatterColor.b.load();
            packedOut[0x3B] = g_oMieScatterColor.a.load();
        }
    }

    if (g_oCloudAlpha.active.load()) {
        packedOut[0x1E] = ClampFloat(g_oCloudAlpha.value.load(), 0.0f, 100.0f);
    }
    if (g_oCloudScatteringCoefficient.active.load()) {
        packedOut[0x20] = ClampFloat(g_oCloudScatteringCoefficient.value.load(), kCloudScatteringCoefficientMin, 100.0f);
    }
    if (g_oCloudFlow.active.load()) {
        packedOut[0x1F] = ClampFloat(g_oCloudFlow.value.load(), 0.0f, 50.0f);
    }
    if (g_oCloudVisibleRange.active.load() && g_windPackBase25Valid.load()) {
        const float mul = ClampFloat(g_oCloudVisibleRange.value.load(), 0.0f, 10.0f);
        packedOut[0x25] = g_windPackBase25.load() * mul;
    }
    if (g_oCloudPhaseFront.active.load()) {
        packedOut[0x21] = ClampFloat(g_oCloudPhaseFront.value.load(), -1.0f, 1.0f);
    }
    if (g_oCloudFadeRange.active.load()) {
        packedOut[0x27] = ClampFloat(g_oCloudFadeRange.value.load(), 0.0f, 200000.0f);
    }
    if (g_oCloudDetailRatio.active.load()) {
        packedOut[0x28] = ClampFloat(g_oCloudDetailRatio.value.load(), 0.0f, 1.5f);
    }

}

void __fastcall Hooked_WeatherFrameUpdate(long long* self, float dt) {
    static bool s_fogOverrideWasActive = false;
    const bool modEnabled = g_modEnabled.load();
    const bool fogWorkNeeded = modEnabled && WeatherFrameFogWorkNeeded();
    const bool restoreNativeFog = s_fogOverrideWasActive && !fogWorkNeeded;

    if (!fogWorkNeeded && !restoreNativeFog) {
        if (g_pOrigWeatherFrameUpdate) g_pOrigWeatherFrameUpdate(self, dt);
        return;
    }

    if (restoreNativeFog) {
        ResetFogBlendWeightsForNativeRefresh(self);
    }

    if (g_pOrigWeatherFrameUpdate) g_pOrigWeatherFrameUpdate(self, dt);

    if (fogWorkNeeded) {
        ForceApplyFogFromFrame(self);
        s_fogOverrideWasActive = true;
    } else if (restoreNativeFog) {
        s_fogOverrideWasActive = false;
    }

    (void)dt;
}

// Clear weather parameters.
static void ApplyClearWeatherParams(long long self, const ResolvedEnv& env) {
    if (!env.valid) return;
    if (env.cloudNode) {
        At<float>(env.cloudNode, CN::FOG_A)       = 0.0f;
        At<float>(env.cloudNode, CN::FOG_B)       = 0.0f;
        At<float>(env.cloudNode, CN::STORM_THRESH) = 0.0f;
        At<float>(env.cloudNode, CN::DUST_THRESH)  = 0.0f;
    }
    At<float>(self, WCO::LERP_ALPHA)     = 1.0f;
    At<float>(self, WCO::BLEND_DIR_MULT) = 1.0f;
}

// Apply all weather parameters after the engine tick.
static void ApplyWeatherParams(long long self, const ResolvedEnv& env) {
    if (!env.valid) return;
    if (env.cloudNode) {
        float fogTotal = g_oFog.get(0.0f);
        float dust = g_oDust.active.load() ? g_oDust.value.load() : 0.0f;
        float nativeDust = DustSliderToNative(dust);
        const bool noRain = g_noRain.load();
        const bool noDust = g_noDust.load();
        const bool noSnow = g_noSnow.load();
        if (g_oFog.active.load()) {
            float fogHalf = fogTotal * 0.5f;
            At<float>(env.cloudNode, CN::FOG_A) = fogHalf;
            At<float>(env.cloudNode, CN::FOG_B) = fogHalf;
        }

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

        if (noRain || noSnow) {
            At<float>(env.cloudNode, CN::STORM_THRESH) = 0.0f;
        }

        if (noDust) {
            At<float>(env.cloudNode, CN::DUST_BASE) = 0.0f;
            At<float>(env.cloudNode, CN::DUST_ADD) = 0.0f;
            At<float>(env.cloudNode, CN::DUST_WIND_SCALE) = 0.0f;
            At<float>(env.cloudNode, CN::DUST_THRESH) = 0.0f;
            At<float>(env.cloudNode, CN::STORM_THRESH) = 0.0f;
        }

        if (!noDust && g_oDust.active.load()) {
            At<float>(env.cloudNode, CN::DUST_BASE) = nativeDust;
            At<float>(env.cloudNode, CN::DUST_ADD) = nativeDust * 0.10f;
            At<float>(env.cloudNode, CN::DUST_WIND_SCALE) = DustSliderToWindScale(dust);
            At<float>(env.cloudNode, CN::DUST_THRESH) = min(At<float>(env.cloudNode, CN::DUST_THRESH), DustSliderToThreshold(dust));
            At<float>(env.cloudNode, CN::STORM_THRESH) = max(At<float>(env.cloudNode, CN::STORM_THRESH), DustSliderToStorm(dust));
            At<float>(env.cloudNode, CN::FOG_B) = max(At<float>(env.cloudNode, CN::FOG_B), DustSliderToFogB(dust));
        }

    }

    ApplyCloudOverrides(env);

    // WeatherComponent lerp overrides.
    At<float>(self, WCO::LERP_ALPHA)     = 1.0f;
    At<float>(self, WCO::BLEND_DIR_MULT) = 1.0f;
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
                   g_oFog.active.load() ||
                   g_oCloudThk.active.load() ||
                   g_oCloudSpdX.active.load() || g_oCloudSpdY.active.load() ||
                   g_oHighClouds.active.load() ||
                   g_oAtmoAlpha.active.load() ||
                   g_oWindActual.active.load();
    return rainDriven && !others;
}

static void TickRainOnly(long long self, const ResolvedEnv& env, int nullSent) {
    if (!g_pActivateEffect || !g_pSetIntensity || !env.particleMgr) return;
    float rain = (!g_noRain.load() && g_oRain.active.load()) ? g_oRain.value.load() : 0.0f;

    if (env.cloudNode) {
        At<float>(env.cloudNode, CN::STORM_THRESH) = rain;
        At<float>(env.cloudNode, CN::DUST_THRESH) = 0.0f;
    }
    At<float>(self, WCO::LERP_ALPHA) = 1.0f;
    At<float>(self, WCO::BLEND_DIR_MULT) = 1.0f;

    auto UpdateEff = [&](int i, bool on, float intensity) {
        int& handle = At<int>(self, WCO::HANDLE_ARRAY + i * 4);
        if (on && handle == nullSent) {
            const EffectSlot& s = kSlots[i];
            g_pActivateEffect(self, s.id,
                reinterpret_cast<long long*>(self + s.slotA),
                reinterpret_cast<long long*>(self + s.slotB), 1.0f);
        }
        int h = At<int>(self, WCO::HANDLE_ARRAY + i * 4);
        if (h != nullSent) g_pSetIntensity(env.particleMgr, h, on ? intensity : 0.0f);
    };

    UpdateEff(0, rain > 0.01f, rain);
    UpdateEff(1, rain > 0.01f, rain);
    UpdateEff(4, rain > 0.5f, max(0.0f, (rain - 0.5f) * 2.0f));
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

static std::atomic<int> g_rainEffectCleanupTicks{ 0 };
static std::atomic<bool> g_rainEffectWasWanted{ false };

static bool RainEffectWantedNow() {
    return !g_forceClear.load() &&
           !g_noRain.load() &&
           g_oRain.active.load() &&
           g_oRain.value.load() > 0.01f;
}

static void RequestRainEffectCleanup(const char* reason) {
    constexpr int kRainCleanupTicks = 30;
    g_rainEffectCleanupTicks.store(kRainCleanupTicks);
    Log("[rain] cleanup requested: %s ticks=%d\n", reason ? reason : "rain disabled", kRainCleanupTicks);
}

static void UpdateRainEffectTransitionCleanup() {
    const bool wanted = RainEffectWantedNow();
    const bool wasWanted = g_rainEffectWasWanted.exchange(wanted);
    if (wasWanted && !wanted) {
        RequestRainEffectCleanup("rain transitioned off");
    }
}

static bool RainEffectCleanupActive() {
    return g_rainEffectCleanupTicks.load() > 0;
}

static void TickRainEffectCleanup(long long self, const ResolvedEnv& env) {
    int ticks = g_rainEffectCleanupTicks.load();
    if (ticks <= 0) {
        return;
    }

    StopWeatherEffectsByMask(self, 0x013u);
    if (env.cloudNode) {
        At<float>(env.cloudNode, CN::STORM_THRESH) = 0.0f;
    }

    ticks = g_rainEffectCleanupTicks.fetch_sub(1) - 1;
    if (ticks <= 0) {
        g_rainEffectCleanupTicks.store(0);
        Log("[rain] cleanup finished\n");
    }
}

static uint32_t ComputeSuppressedWeatherEffectMask() {
    uint32_t mask = 0;
    if (g_noRain.load()) mask |= 0x013u;
    if (g_noSnow.load()) mask |= 0x00Cu;
    if (g_noDust.load()) mask |= 0x040u;
    return mask;
}

static void ApplyNoWindPolicy(long long self, const ResolvedEnv& env) {
    At<int>(self, WCO::SOUND_WIND)    = 0;
    At<int>(self, WCO::SOUND_SKYWIND) = 0;
    if (env.windNode) {
        At<float>(env.windNode, WN::SPEED) = 0.0f;
        At<float>(env.windNode, WN::GUST)  = 0.0f;
    }
}

static void ApplyDustWindPolicy(long long self, const ResolvedEnv& env) {
    if (!env.valid || !DustForcesCalmWind()) return;
    At<int>(self, WCO::SOUND_WIND)    = 0;
    At<int>(self, WCO::SOUND_SKYWIND) = 0;
    if (env.windNode) {
        At<float>(env.windNode, WN::SPEED) = 0.0f;
        At<float>(env.windNode, WN::GUST)  = 0.0f;
    }
}

// Process wind state hook.
void __fastcall Hooked_ProcessWindState(long long self) {
    if (g_pOrigProcessWindState) g_pOrigProcessWindState(self);
    if (!g_modEnabled.load()) return;
    const ResolvedEnv env = ResolveEnv();
    if (env.valid) {
        CaptureCloudBaseline(env);
        ApplyCloudOverrides(env);
    }
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
        TickRainOnly(self, env, nullSent);
        return;
    }

    ApplyWeatherParams(self, env);
    uint32_t mask = ComputeCustomEffectMask();

    if (g_pActivateEffect && g_pSetIntensity && env.particleMgr) {
        for (int i = 0; i < kEffectCount; i++) {
            int& handle = At<int>(self, WCO::HANDLE_ARRAY + i * 4);
            if (mask & (1u << i)) {
                const EffectSlot& s = kSlots[i];
                if (handle == nullSent) {
                    g_pActivateEffect(self, s.id,
                        reinterpret_cast<long long*>(self + s.slotA),
                        reinterpret_cast<long long*>(self + s.slotB), 1.0f);
                }
                int h = At<int>(self, WCO::HANDLE_ARRAY + i * 4);
                if (h != nullSent) {
                    float effI = 1.0f;
                    if (i <= 1) effI = (!g_noRain.load() && g_oRain.active.load()) ? g_oRain.value.load() : 0.0f;
                    else if (i == 2 || i == 3) effI = (!g_noSnow.load() && g_oSnow.active.load()) ? g_oSnow.value.load() : 0.0f;
                    else if (i == 4) effI = (!g_noRain.load() && g_oRain.active.load()) ? max(0.f, g_oRain.value.load() - 0.5f) * 2.f : 0.0f;
                    else if (i == 5) effI = g_oWindActual.active.load() ? min(1.f, g_oWindActual.value.load() / 10.f) : 0.0f;
                    else if (i == 6) effI = (!g_noDust.load() && g_oDust.active.load()) ? min(1.0f, g_oDust.value.load()) : 0.0f;
                    else if (i == 7) effI = min(
                        (!g_noSnow.load() && g_oSnow.active.load()) ? g_oSnow.value.load() : 0.0f,
                        (!g_noDust.load() && g_oDust.active.load()) ? g_oDust.value.load() : 0.0f);
                    else if (i == 8) effI = 0.0f;
                    g_pSetIntensity(env.particleMgr, h, effI);
                }
            } else if (handle != nullSent) {
                g_pSetIntensity(env.particleMgr, handle, 0.0f);
            }
        }
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
}

namespace {

constexpr uintptr_t kGameClockBaseMsRva = 0x6067108;
constexpr uintptr_t kGameClockElapsedBaseMsRva = 0x5F525A8;
constexpr uintptr_t kGameClockAccumUsRva = 0x5F52598;
constexpr uintptr_t kGameClockSnapshotPrimaryRva = 0x5D31F08;
constexpr uintptr_t kGameClockSnapshotTlsRva = 0x5D31F28;
constexpr unsigned long long kNativeClockDayMs = 0x4819080ull;
constexpr unsigned long long kNativeClockDawnBendMs = 0x5265C0ull;
constexpr unsigned long long kNativeClockNightBendMs = 0x42F2AC0ull;
std::atomic<long long> g_realGameTimePendingFieldShiftTicks{ 0 };
std::atomic<unsigned long long> g_realGameTimePendingFieldSyncId{ 0 };
std::atomic<unsigned long long> g_realGameTimePendingFieldSyncUntilMs{ 0 };
std::atomic<unsigned long long> g_realGameTimeFieldSyncWriteCount{ 0 };
std::atomic<unsigned long long> g_realGameTimeSnapshotActionWriteCount{ 0 };
std::atomic<unsigned long long> g_realGameTimeFieldGlobalWriteCount{ 0 };
std::atomic<long long> g_realGameTimeVirtualAnchorTick{ -1 };
std::atomic<long long> g_realGameTimeVirtualAnchorTimestampMs{ 0 };
std::atomic<unsigned int> g_realGameTimeLastRate{ 12 };
std::atomic<unsigned long long> g_realGameTimeStaleSnapshotFixCount{ 0 };

struct GameClockCalendar {
    int day = -1;
    int hour = -1;
    int minute = -1;
    int second = -1;
    int millisecond = -1;
};

bool ReadGameClockGlobals(long long& outBaseMs, long long& outAccumUs, bool& outActive) {
    const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!moduleBase) {
        return false;
    }

    __try {
        const long long anchorMs = *reinterpret_cast<long long*>(moduleBase + kGameClockBaseMsRva);
        const long long elapsedBaseMs = *reinterpret_cast<long long*>(moduleBase + kGameClockElapsedBaseMsRva);
        outBaseMs = anchorMs + elapsedBaseMs;
        outAccumUs = *reinterpret_cast<long long*>(moduleBase + kGameClockAccumUsRva);
        outActive = elapsedBaseMs != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

GameClockCalendar ConvertGameClockMs(long long clockMs) {
    GameClockCalendar out;
    if (clockMs < 0) {
        return out;
    }

    unsigned long long withinDay = static_cast<unsigned long long>(clockMs) % kNativeClockDayMs;
    out.day = static_cast<int>(static_cast<unsigned long long>(clockMs) / kNativeClockDayMs);
    unsigned long long visualMs = 0;
    if (withinDay >= kNativeClockDawnBendMs) {
        visualMs = withinDay >= kNativeClockNightBendMs
            ? (2ull * withinDay) - 64800000ull
            : withinDay + 5400000ull;
    } else {
        visualMs = 2ull * withinDay;
    }

    const unsigned long long totalSeconds = visualMs / 1000ull;
    out.hour = static_cast<int>((totalSeconds / 3600ull) % 24ull);
    out.minute = static_cast<int>((totalSeconds / 60ull) % 60ull);
    out.second = static_cast<int>(totalSeconds % 60ull);
    out.millisecond = static_cast<int>(visualMs % 1000ull);
    return out;
}

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

long long CalendarToLinearMs(const GameClockCalendar& cal) {
    const long long extraDays = cal.hour >= 24 ? (cal.hour / 24) : 0;
    const long long hour = cal.hour >= 24 ? (cal.hour % 24) : cal.hour;
    return ((((static_cast<long long>(cal.day) + extraDays) * 24ll + hour) * 60ll + cal.minute) * 60ll + cal.second) * 1000ll +
        cal.millisecond;
}

GameClockCalendar LinearMsToCalendar(long long linearMs) {
    if (linearMs < 0) {
        linearMs = 0;
    }

    GameClockCalendar out;
    out.millisecond = static_cast<int>(linearMs % 1000ll);
    long long totalSeconds = linearMs / 1000ll;
    out.second = static_cast<int>(totalSeconds % 60ll);
    long long totalMinutes = totalSeconds / 60ll;
    out.minute = static_cast<int>(totalMinutes % 60ll);
    long long totalHours = totalMinutes / 60ll;
    out.hour = static_cast<int>(totalHours % 24ll);
    out.day = static_cast<int>(totalHours / 24ll);
    return out;
}

long long CalendarToNativeClockTicks(const GameClockCalendar& cal) {
    const long long extraDays = cal.hour >= 24 ? (cal.hour / 24) : 0;
    const long long hour = cal.hour >= 24 ? (cal.hour % 24) : cal.hour;
    unsigned long long visualMs =
        static_cast<unsigned long long>(cal.millisecond) +
        1000ull * (static_cast<unsigned long long>(cal.second) +
        60ull * (static_cast<unsigned long long>(cal.minute) +
        60ull * static_cast<unsigned long long>(hour)));
    unsigned long long withinDay = visualMs;
    if (visualMs >= 10800000ull) {
        if (visualMs < 75600000ull) {
            withinDay = visualMs - 5400000ull;
        } else {
            withinDay = (visualMs + 64800000ull) >> 1;
        }
    } else {
        withinDay = visualMs >> 1;
    }
    return static_cast<long long>(withinDay + kNativeClockDayMs * static_cast<unsigned long long>(cal.day + extraDays));
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

void SyncGameClockActionSnapshots(const GameClockCalendar& targetCal,
                                  unsigned short rate,
                                  long long clockTimestampMs,
                                  const char* action) {
    const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!moduleBase || targetCal.day < 0) {
        return;
    }

    WriteGameClockSnapshotStruct(static_cast<long long>(moduleBase + kGameClockSnapshotPrimaryRva),
                                 targetCal,
                                 rate,
                                 clockTimestampMs);
    WriteGameClockSnapshotStruct(static_cast<long long>(moduleBase + kGameClockSnapshotTlsRva),
                                 targetCal,
                                 rate,
                                 clockTimestampMs);

    const unsigned long long writes = g_realGameTimeSnapshotActionWriteCount.fetch_add(1) + 1;
    Log("[real-time] snapshot-action %s target=day %d %02d:%02d:%02d.%03d rate=%u timestamp=%lld writes=%llu\n",
        action ? action : "commit",
        targetCal.day,
        targetCal.hour,
        targetCal.minute,
        targetCal.second,
        targetCal.millisecond,
        static_cast<unsigned>(rate ? rate : 1),
        clockTimestampMs,
        writes);
}

unsigned short ReadGameClockRateStruct(long long storage) {
    if (!storage) {
        return 0;
    }

    __try {
        return *reinterpret_cast<unsigned short*>(storage + 0x16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void RememberGameClockRate(unsigned short rate) {
    if (rate != 0) {
        g_realGameTimeLastRate.store(static_cast<unsigned int>(rate));
    }
}

unsigned short GetRememberedGameClockRate(unsigned short fallback) {
    if (fallback != 0) {
        return fallback;
    }
    const unsigned int remembered = g_realGameTimeLastRate.load();
    return static_cast<unsigned short>(remembered ? remembered : 12u);
}

bool ReadEffectiveGameClockMs(long long& outClockMs) {
    long long clockMs = 0;
    long long accumUs = 0;
    bool active = false;
    if (!ReadGameClockGlobals(clockMs, accumUs, active)) {
        return false;
    }
    outClockMs = clockMs + (active ? accumUs / 1000ll : 0ll);
    return true;
}

void ResetRealGameTimeVirtualAnchor() {
    g_realGameTimeVirtualAnchorTick.store(-1);
    g_realGameTimeVirtualAnchorTimestampMs.store(0);
}

void WriteGameClockFallbackSnapshots(const GameClockCalendar& cal,
                                     unsigned short rate,
                                     long long timestampMs,
                                     const char* reason) {
    const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!moduleBase || cal.day < 0) {
        return;
    }

    WriteGameClockSnapshotStruct(static_cast<long long>(moduleBase + kGameClockSnapshotPrimaryRva),
                                 cal,
                                 rate,
                                 timestampMs);
    WriteGameClockSnapshotStruct(static_cast<long long>(moduleBase + kGameClockSnapshotTlsRva),
                                 cal,
                                 rate,
                                 timestampMs);
    (void)reason;
}

void SetRealGameTimeVirtualAnchor(const GameClockCalendar& cal,
                                  unsigned short rate,
                                  long long timestampMs,
                                  const char* reason,
                                  bool writeSnapshots) {
    if (cal.day < 0) {
        return;
    }

    const long long tick = CalendarToNativeClockTicks(cal);
    g_realGameTimeVirtualAnchorTick.store(tick);
    g_realGameTimeVirtualAnchorTimestampMs.store(timestampMs);
    RememberGameClockRate(rate);
    if (writeSnapshots) {
        WriteGameClockFallbackSnapshots(cal, rate, timestampMs, reason);
    }
}

bool ApplyRealGameTimeSnapshotGuard(unsigned char* source, long long outTime, GameClockCalendar& nativeCal) {
    if (!g_modEnabled.load() || !g_realGameTimeEnabled.load() || !outTime) {
        ResetRealGameTimeVirtualAnchor();
        return false;
    }

    long long effectiveClockMs = 0;
    if (!ReadEffectiveGameClockMs(effectiveClockMs)) {
        return false;
    }

    unsigned short rate = GetRememberedGameClockRate(ReadGameClockRateStruct(outTime));
    if (rate == 0) {
        rate = 1;
    }
    RememberGameClockRate(rate);

    const long long nativeTick = CalendarToNativeClockTicks(nativeCal);
    long long anchorTick = g_realGameTimeVirtualAnchorTick.load();
    if (anchorTick < 0) {
        SetRealGameTimeVirtualAnchor(nativeCal, rate, effectiveClockMs, "init", true);
        return false;
    }

    const long long expectedTick = max(0ll, anchorTick);
    const long long staleThresholdTicks = max(24000ll, static_cast<long long>(rate) * 2000ll);
    if (nativeTick + staleThresholdTicks >= expectedTick) {
        return false;
    }

    const GameClockCalendar staleCal = nativeCal;
    const GameClockCalendar expectedCal = ConvertGameClockMs(expectedTick);
    if (expectedCal.day < 0) {
        return false;
    }

    WriteGameClockSnapshotStruct(outTime, expectedCal, rate, effectiveClockMs);
    SetRealGameTimeVirtualAnchor(expectedCal, rate, effectiveClockMs, "stale-fix", true);
    nativeCal = expectedCal;

    const unsigned long long fixes = g_realGameTimeStaleSnapshotFixCount.fetch_add(1) + 1;
    if (fixes <= 24 || (fixes % 120) == 0) {
        Log("[real-time] stale-snapshot source=%p out=%p native=day %d %02d:%02d:%02d.%03d expected=day %d %02d:%02d:%02d.%03d deltaTicks=%lld rate=%u effective=%lld fixes=%llu\n",
            source,
            reinterpret_cast<void*>(outTime),
            staleCal.day,
            staleCal.hour,
            staleCal.minute,
            staleCal.second,
            staleCal.millisecond,
            expectedCal.day,
            expectedCal.hour,
            expectedCal.minute,
            expectedCal.second,
            expectedCal.millisecond,
            expectedTick - nativeTick,
            static_cast<unsigned>(rate),
            effectiveClockMs,
            fixes);
    }
    return true;
}

bool AdvanceRealGameTimeVirtualAnchor(long long nativeStepMs,
                                      double speed,
                                      long long timestampMs,
                                      double& fractionalTicks) {
    if (nativeStepMs <= 0) {
        return false;
    }

    const long long currentTick = g_realGameTimeVirtualAnchorTick.load();
    if (currentTick < 0) {
        return false;
    }

    const unsigned short rate = GetRememberedGameClockRate(0);
    const double exactStep = (static_cast<double>(nativeStepMs) *
                              static_cast<double>(rate ? rate : 1) *
                              speed) + fractionalTicks;
    const long long stepTicks = static_cast<long long>(std::llround(exactStep));
    fractionalTicks = exactStep - static_cast<double>(stepTicks);
    if (stepTicks <= 0) {
        return false;
    }

    const long long nextTick = max(0ll, currentTick + stepTicks);
    const GameClockCalendar nextCal = ConvertGameClockMs(nextTick);
    if (nextCal.day < 0) {
        return false;
    }

    g_realGameTimeVirtualAnchorTick.store(nextTick);
    g_realGameTimeVirtualAnchorTimestampMs.store(timestampMs);
    WriteGameClockFallbackSnapshots(nextCal, rate ? rate : 1, timestampMs, "advance");
    return true;
}

bool ResolveGameTimeFieldStorage(unsigned char* source, long long& outStorage, unsigned short& outAreaId) {
    outStorage = 0;
    outAreaId = 0xFFFF;
    if (!source || !g_pGameFieldInfoResolver) {
        return false;
    }

    __try {
        void** vtable = *reinterpret_cast<void***>(source);
        if (!vtable || !vtable[11]) {
            return false;
        }
        using GetAreaIdFn = void(__fastcall*)(unsigned char*, unsigned short*);
        reinterpret_cast<GetAreaIdFn>(vtable[11])(source, &outAreaId);
        if (outAreaId == 0xFFFF) {
            return false;
        }

        const long long fieldInfo = g_pGameFieldInfoResolver(&outAreaId);
        if (!fieldInfo || !*reinterpret_cast<unsigned char*>(fieldInfo + 0x64)) {
            return false;
        }
        outStorage = fieldInfo + 0x40;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outStorage = 0;
        outAreaId = 0xFFFF;
        return false;
    }
}

bool ProbeGameTimeFieldStorage(unsigned char* source,
                               long long& outStorage,
                               unsigned short& outAreaId,
                               const char** outReason) {
    outStorage = 0;
    outAreaId = 0xFFFF;
    if (outReason) {
        *outReason = "ok";
    }
    if (!source) {
        if (outReason) {
            *outReason = "source-null";
        }
        return false;
    }
    if (!g_pGameFieldInfoResolver) {
        if (outReason) {
            *outReason = "resolver-null";
        }
        return false;
    }

    __try {
        void** vtable = *reinterpret_cast<void***>(source);
        if (!vtable) {
            if (outReason) {
                *outReason = "vtable-null";
            }
            return false;
        }
        if (!vtable[11]) {
            if (outReason) {
                *outReason = "area-fn-null";
            }
            return false;
        }

        using GetAreaIdFn = void(__fastcall*)(unsigned char*, unsigned short*);
        reinterpret_cast<GetAreaIdFn>(vtable[11])(source, &outAreaId);
        if (outAreaId == 0xFFFF) {
            if (outReason) {
                *outReason = "area-invalid";
            }
            return false;
        }

        const long long fieldInfo = g_pGameFieldInfoResolver(&outAreaId);
        if (!fieldInfo) {
            if (outReason) {
                *outReason = "field-null";
            }
            return false;
        }
        if (!*reinterpret_cast<unsigned char*>(fieldInfo + 0x64)) {
            if (outReason) {
                *outReason = "field-disabled";
            }
            return false;
        }
        outStorage = fieldInfo + 0x40;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        outStorage = 0;
        outAreaId = 0xFFFF;
        if (outReason) {
            *outReason = "exception";
        }
        return false;
    }
}

void ArmRealGameTimeFieldSync(const GameClockCalendar& nativeCal, const GameClockCalendar& targetCal) {
    const long long nativeTick = CalendarToNativeClockTicks(nativeCal);
    const long long targetTick = CalendarToNativeClockTicks(targetCal);
    const long long shiftTicks = targetTick - nativeTick;
    if (!shiftTicks) {
        return;
    }

    g_realGameTimePendingFieldShiftTicks.store(shiftTicks);
    g_realGameTimePendingFieldSyncUntilMs.store(GetTickCount64() + 5000ull);
    g_realGameTimePendingFieldSyncId.fetch_add(1);
}

bool ApplyPendingRealGameTimeFieldSync(unsigned char* source, long long outTime, const GameClockCalendar& nativeCal) {
    const unsigned long long syncId = g_realGameTimePendingFieldSyncId.load();
    if (!syncId || GetTickCount64() > g_realGameTimePendingFieldSyncUntilMs.load()) {
        return false;
    }

    long long storage = 0;
    unsigned short areaId = 0xFFFF;
    const char* failReason = "unknown";
    if (!ProbeGameTimeFieldStorage(source, storage, areaId, &failReason)) {
        (void)failReason;
        return false;
    }

    static std::array<unsigned long long, 8> s_storage{};
    static std::array<unsigned long long, 8> s_syncId{};
    size_t slot = 0;
    for (size_t i = 0; i < s_storage.size(); ++i) {
        if (s_storage[i] == static_cast<unsigned long long>(storage)) {
            if (s_syncId[i] == syncId) {
                return false;
            }
            slot = i;
            break;
        }
        if (!s_storage[i]) {
            slot = i;
            break;
        }
    }

    const long long shiftedTick = max(0ll, CalendarToNativeClockTicks(nativeCal) + g_realGameTimePendingFieldShiftTicks.load());
    const GameClockCalendar target = ConvertGameClockMs(shiftedTick);
    WriteGameClockCalendarStruct(storage, target);
    WriteGameClockCalendarStruct(outTime, target);
    s_storage[slot] = static_cast<unsigned long long>(storage);
    s_syncId[slot] = syncId;

    const unsigned long long writes = g_realGameTimeFieldSyncWriteCount.fetch_add(1) + 1;
    if (writes <= 8 || (writes % 32) == 0) {
        Log("[real-time] field-sync area=%u storage=%p native=day %d %02d:%02d:%02d.%03d target=day %d %02d:%02d:%02d.%03d sync=%llu writes=%llu\n",
            static_cast<unsigned>(areaId),
            reinterpret_cast<void*>(storage),
            nativeCal.day,
            nativeCal.hour,
            nativeCal.minute,
            nativeCal.second,
            nativeCal.millisecond,
            target.day,
            target.hour,
            target.minute,
            target.second,
            target.millisecond,
            syncId,
            writes);
    }
    return true;
}

bool SyncRealGameTimeFieldToGlobalClock(unsigned char* source, long long outTime) {
    if (!g_modEnabled.load() || !g_realGameTimeEnabled.load()) {
        return false;
    }

    long long storage = 0;
    unsigned short areaId = 0xFFFF;
    if (!ResolveGameTimeFieldStorage(source, storage, areaId)) {
        return false;
    }

    long long currentClockMs = 0;
    long long currentAccumUs = 0;
    bool currentActive = false;
    if (!ReadGameClockGlobals(currentClockMs, currentAccumUs, currentActive)) {
        return false;
    }
    const long long effectiveClockMs = currentClockMs + (currentActive ? currentAccumUs / 1000ll : 0ll);
    const GameClockCalendar target = ConvertGameClockMs(effectiveClockMs);
    if (target.day < 0) {
        return false;
    }

    unsigned short rate = 0;
    __try {
        rate = *reinterpret_cast<unsigned short*>(storage + 0x16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        rate = 0;
    }
    if (!rate && outTime) {
        __try {
            rate = *reinterpret_cast<unsigned short*>(outTime + 0x16);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            rate = 0;
        }
    }
    if (!rate) {
        rate = 1;
    }

    WriteGameClockSnapshotStruct(storage, target, rate, effectiveClockMs);
    WriteGameClockSnapshotStruct(outTime, target, rate, effectiveClockMs);

    const unsigned long long writes = g_realGameTimeFieldGlobalWriteCount.fetch_add(1) + 1;
    if (writes <= 8 || (writes % 300) == 0) {
        Log("[real-time] field-global area=%u storage=%p target=day %d %02d:%02d:%02d.%03d rate=%u timestamp=%lld writes=%llu\n",
            static_cast<unsigned>(areaId),
            reinterpret_cast<void*>(storage),
            target.day,
            target.hour,
            target.minute,
            target.second,
            target.millisecond,
            static_cast<unsigned>(rate),
            effectiveClockMs,
            writes);
    }
    return true;
}

bool CommitGlobalGameTime(long long outTime,
                          const GameClockCalendar& nativeCal,
                          const GameClockCalendar& targetCal,
                          const char* action) {
    const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!moduleBase || !outTime) {
        return false;
    }

    unsigned short rate = 0;
    __try {
        rate = *reinterpret_cast<unsigned short*>(outTime + 0x16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (rate == 0) {
        rate = 1;
    }
    RememberGameClockRate(rate);

    const long long nativeTick = CalendarToNativeClockTicks(nativeCal);
    const long long targetTick = CalendarToNativeClockTicks(targetCal);
    long long currentClockMs = 0;
    long long currentAccumUs = 0;
    bool currentActive = false;
    if (!ReadGameClockGlobals(currentClockMs, currentAccumUs, currentActive)) {
        return false;
    }
    const long long effectiveClockMs = currentClockMs + (currentActive ? currentAccumUs / 1000ll : 0ll);
    const long long clockShift = (targetTick - nativeTick) / static_cast<long long>(rate);
    __try {
        *reinterpret_cast<long long*>(moduleBase + kGameClockBaseMsRva) += clockShift;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    SyncGameClockActionSnapshots(targetCal, rate, effectiveClockMs + clockShift, action);
    SetRealGameTimeVirtualAnchor(targetCal, rate, effectiveClockMs + clockShift, action, false);

    const unsigned long long writes = g_realGameTimeWriteCount.fetch_add(1) + 1;
    Log("[real-time] %s native=day %d %02d:%02d:%02d.%03d target=day %d %02d:%02d:%02d.%03d rate=%u shift=%lld base=%lld accum=%lld active=%u timestamp=%lld writes=%llu\n",
        action ? action : "commit",
        nativeCal.day,
        nativeCal.hour,
        nativeCal.minute,
        nativeCal.second,
        nativeCal.millisecond,
        targetCal.day,
        targetCal.hour,
        targetCal.minute,
        targetCal.second,
        targetCal.millisecond,
        static_cast<unsigned>(rate),
        clockShift,
        currentClockMs,
        currentAccumUs,
        currentActive ? 1u : 0u,
        effectiveClockMs + clockShift,
        writes);
    return true;
}

bool ApplyRealGameTimeAction(unsigned char* source, long long outTime, const GameClockCalendar& nativeCal) {
    if (!g_modEnabled.load() || !g_realGameTimeEnabled.load()) {
        return false;
    }

    const int dayDelta = g_realGameTimeDayDeltaRequest.exchange(0);
    const int minuteRequest = g_realGameTimeSetMinuteRequest.exchange(-1);
    if (dayDelta == 0 && minuteRequest < 0) {
        return false;
    }

    GameClockCalendar target = nativeCal;
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
    if (!CommitGlobalGameTime(outTime, nativeCal, target, dayDelta ? "day-step" : "clock-set")) {
        return false;
    }
    long long fieldStorage = 0;
    unsigned short areaId = 0xFFFF;
    if (ResolveGameTimeFieldStorage(source, fieldStorage, areaId)) {
        WriteGameClockCalendarStruct(fieldStorage, target);
        Log("[real-time] field-action area=%u storage=%p target=day %d %02d:%02d:%02d.%03d\n",
            static_cast<unsigned>(areaId),
            reinterpret_cast<void*>(fieldStorage),
            target.day,
            target.hour,
            target.minute,
            target.second,
            target.millisecond);
    }
    ArmRealGameTimeFieldSync(nativeCal, target);
    WriteGameClockCalendarStruct(outTime, target);
    return true;
}

long long ApplyRealGameTimeScale(long long beforeClockMs,
                                 long long afterClockMs,
                                 long long afterAccumUs,
                                 bool active) {
    static double s_fractionalShift = 0.0;
    static double s_fractionalVirtualTicks = 0.0;
    static long long s_lastAccumUs = 0;
    (void)beforeClockMs;
    if (!g_modEnabled.load() || !g_realGameTimeEnabled.load() || !active) {
        s_fractionalShift = 0.0;
        s_fractionalVirtualTicks = 0.0;
        s_lastAccumUs = 0;
        ResetRealGameTimeVirtualAnchor();
        return afterClockMs;
    }

    const long long accumDeltaUs = s_lastAccumUs ? (afterAccumUs - s_lastAccumUs) : 0;
    s_lastAccumUs = afterAccumUs;
    if (accumDeltaUs <= 0 || accumDeltaUs > 1000000ll) {
        if (accumDeltaUs < 0 || accumDeltaUs > 1000000ll) {
            s_fractionalShift = 0.0;
            s_fractionalVirtualTicks = 0.0;
        }
        return afterClockMs;
    }
    const long long nativeStep = accumDeltaUs / 1000ll;
    if (nativeStep <= 0) {
        return afterClockMs;
    }

    const int hour = g_gameTimeProbeHour.load();
    if (hour < 0 || hour >= 24) {
        return afterClockMs;
    }

    const bool isDay = hour >= 3 && hour < 19;
    const float selectedScale = isDay
        ? g_realGameTimeDayScale.load()
        : g_realGameTimeNightScale.load();
    const double speed = static_cast<double>(min(60.0f, max(0.01f, selectedScale)));
    long long resultClockMs = afterClockMs;
    const double exactShift = static_cast<double>(nativeStep) * (speed - 1.0) + s_fractionalShift;
    const long long clockShift = static_cast<long long>(std::llround(exactShift));
    s_fractionalShift = exactShift - static_cast<double>(clockShift);

    if (clockShift) {
        const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (moduleBase) {
            __try {
                *reinterpret_cast<long long*>(moduleBase + kGameClockBaseMsRva) += clockShift;
                resultClockMs = afterClockMs + clockShift;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                resultClockMs = afterClockMs;
            }
        }
    }

    AdvanceRealGameTimeVirtualAnchor(nativeStep, speed, resultClockMs, s_fractionalVirtualTicks);
    const unsigned long long writes = g_realGameTimeScaleWriteCount.fetch_add(1) + 1;
    if (clockShift && (writes <= 16 || (writes % 300) == 0)) {
        Log("[real-time] scale-write nativeStep=%lldms accumDelta=%lldus speed=%.3f shift=%lld after=%lld result=%lld writes=%llu\n",
            nativeStep,
            accumDeltaUs,
            speed,
            clockShift,
            afterClockMs,
            resultClockMs,
            writes);
    }
    return resultClockMs;
}

bool ResolveNativeGameTimeStorage(unsigned char* source, long long& outStorage, unsigned short& outAreaId) {
#if defined(CW_DEV_BUILD)
    return ResolveGameTimeFieldStorage(source, outStorage, outAreaId);
#else
    (void)source;
    outStorage = 0;
    outAreaId = 0xFFFF;
    return false;
#endif
}

bool WriteNativeGameTimeStorage(unsigned char* source,
                                long long outTime,
                                const GameClockCalendar& nativeCal,
                                const GameClockCalendar& virtualCal,
                                bool allowGlobalFallback) {
#if defined(CW_DEV_BUILD)
    long long storage = 0;
    unsigned short areaId = 0xFFFF;
    if (ResolveNativeGameTimeStorage(source, storage, areaId)) {
        WriteGameClockCalendarStruct(storage, virtualCal);
        g_devGameTimeLastStorage.store(static_cast<unsigned long long>(storage));
        const unsigned long long writes = g_devGameTimeWriteCount.fetch_add(1) + 1;
        if (writes <= 8 || (writes % 300) == 0) {
            Log("[time-probe] DEV writeback field area=%u storage=%p time=day %d %02d:%02d:%02d.%03d writes=%llu\n",
                static_cast<unsigned>(areaId),
                reinterpret_cast<void*>(storage),
                virtualCal.day,
                virtualCal.hour,
                virtualCal.minute,
                virtualCal.second,
                virtualCal.millisecond,
                writes);
        }
        return true;
    }

    if (!allowGlobalFallback) {
        return false;
    }

    const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!moduleBase || !outTime) {
        return false;
    }
    unsigned short rate = 0;
    __try {
        rate = *reinterpret_cast<unsigned short*>(outTime + 0x16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (rate == 0) {
        rate = 1;
    }
    const long long nativeTick = CalendarToNativeClockTicks(nativeCal);
    const long long virtualTick = CalendarToNativeClockTicks(virtualCal);
    const long long clockShift = (virtualTick - nativeTick) / static_cast<long long>(rate);
    __try {
        long long* baseMs = reinterpret_cast<long long*>(moduleBase + kGameClockBaseMsRva);
        *baseMs += clockShift;
        storage = reinterpret_cast<long long>(baseMs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    g_devGameTimeLastStorage.store(static_cast<unsigned long long>(storage));
    const unsigned long long writes = g_devGameTimeWriteCount.fetch_add(1) + 1;
    if (writes <= 8 || (writes % 300) == 0) {
        Log("[time-probe] DEV writeback global base=%p rate=%u native=day %d %02d:%02d:%02d.%03d virtual=day %d %02d:%02d:%02d.%03d shift=%lld writes=%llu\n",
            reinterpret_cast<void*>(storage),
            static_cast<unsigned>(rate),
            nativeCal.day,
            nativeCal.hour,
            nativeCal.minute,
            nativeCal.second,
            nativeCal.millisecond,
            virtualCal.day,
            virtualCal.hour,
            virtualCal.minute,
            virtualCal.second,
            virtualCal.millisecond,
            clockShift,
            writes);
    }
    return true;
#else
    (void)source;
    (void)outTime;
    (void)nativeCal;
    (void)virtualCal;
    return false;
#endif
}

bool ApplyDevGameTimeOverride(unsigned char* source, long long outTime, const GameClockCalendar& nativeCal) {
#if defined(CW_DEV_BUILD)
    if (!g_devGameTimeOverrideEnabled.load()) {
        g_devGameTimeAnchorValid.store(false);
        return false;
    }

    const long long nativeMs = CalendarToLinearMs(nativeCal);
    long long virtualMs = nativeMs;
    const int mode = g_devGameTimeOverrideMode.load();
    if (mode == 0) {
        virtualMs = nativeMs + static_cast<long long>(g_devGameTimeOffsetMinutes.load()) * 60ll * 1000ll;
    } else if (mode == 1) {
        const int fixedHour = std::clamp(g_devGameTimeFixedHour.load(), 0, 23);
        const int fixedMinute = std::clamp(g_devGameTimeFixedMinute.load(), 0, 59);
        virtualMs = (((static_cast<long long>(nativeCal.day) * 24ll + fixedHour) * 60ll + fixedMinute) * 60ll) * 1000ll;
    } else {
        if (g_devGameTimeResetAnchor.exchange(false) || !g_devGameTimeAnchorValid.load()) {
            g_devGameTimeAnchorNativeMs.store(nativeMs);
            g_devGameTimeAnchorVirtualMs.store(nativeMs);
            g_devGameTimeAnchorValid.store(true);
        }
        const long long anchorNativeMs = g_devGameTimeAnchorNativeMs.load();
        const long long anchorVirtualMs = g_devGameTimeAnchorVirtualMs.load();
        const int targetMinuteMs = std::max(1, g_devGameTimeMinuteMs.load());
        const float sliderScale = std::max(0.0f, g_devGameTimeScale.load());
        const double scaleFromMinuteMs = 5000.0 / static_cast<double>(targetMinuteMs);
        const double effectiveScale = sliderScale * scaleFromMinuteMs;
        virtualMs = anchorVirtualMs + static_cast<long long>(static_cast<double>(nativeMs - anchorNativeMs) * effectiveScale);
    }

    const GameClockCalendar virtualCal = LinearMsToCalendar(virtualMs);
    const bool commitOnce = g_devGameTimeCommitOnce.exchange(false);
    const bool shouldWriteNative = g_devGameTimeWriteNative.load() || commitOnce;
    if (shouldWriteNative) {
        WriteNativeGameTimeStorage(source, outTime, nativeCal, virtualCal, commitOnce);
    }
    WriteGameClockCalendarStruct(outTime, virtualCal);
    g_devGameTimeLastNativeMs.store(nativeMs);
    g_devGameTimeLastVirtualMs.store(virtualMs);
    const unsigned long long calls = g_devGameTimeOverrideCallCount.fetch_add(1) + 1;
    if (calls <= 8 || (calls % 300) == 0) {
        Log("[time-probe] DEV override mode=%d native=%02d:%02d:%02d.%03d virtual=%02d:%02d:%02d.%03d calls=%llu\n",
            mode,
            nativeCal.hour,
            nativeCal.minute,
            nativeCal.second,
            nativeCal.millisecond,
            virtualCal.hour,
            virtualCal.minute,
            virtualCal.second,
            virtualCal.millisecond,
            calls);
    }
    return true;
#else
    (void)source;
    (void)outTime;
    (void)nativeCal;
    return false;
#endif
}

void StoreGameTimeProbeSample(long long beforeClockMs,
                              long long beforeAccumUs,
                              unsigned long long beforeRealtimeMs,
                              long long afterClockMs,
                              long long afterAccumUs,
                              bool active) {
    const unsigned long long now = GetTickCount64();
    const unsigned long long calls = g_gameTimeProbeCallCount.fetch_add(1) + 1;
    const unsigned long long lastRealtimeMs = g_gameTimeProbeLastRealtimeMs.exchange(now);
    const unsigned long long elapsedRealtimeMs = (lastRealtimeMs && now >= lastRealtimeMs) ? (now - lastRealtimeMs) : 0;
    const long long deltaClockMs = afterClockMs - beforeClockMs;
    const long long deltaAccumUs = afterAccumUs - beforeAccumUs;

    g_gameTimeProbeLastFrameMs.store(elapsedRealtimeMs);
    g_gameTimeProbeLastClockMs.store(afterClockMs);
    g_gameTimeProbeLastDeltaClockMs.store(deltaClockMs);
    g_gameTimeProbeLastAccumUs.store(afterAccumUs);
    g_gameTimeProbeLastDeltaAccumUs.store(deltaAccumUs);
    g_gameTimeProbeAccumulatorActive.store(active);
    if (elapsedRealtimeMs > 0) {
        g_gameTimeProbeLastSpeed.store(static_cast<float>(deltaClockMs) / static_cast<float>(elapsedRealtimeMs));
    }
    (void)calls;
    (void)beforeRealtimeMs;
}

void StoreGameTimeGetterSample(unsigned char* source, long long outTime) {
    GameClockCalendar cal;
    if (!ReadGameClockCalendarStruct(outTime, cal)) {
        return;
    }

    const unsigned long long now = GetTickCount64();
    const unsigned long long calls = g_gameTimeProbeGetterCallCount.fetch_add(1) + 1;
    const unsigned long long lastRealtimeMs = g_gameTimeProbeLastGetterRealtimeMs.exchange(now);
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

long long __fastcall Hooked_GameTimeUpdate(long long self, long long eventContext, long long* timeContext, long long outTime) {
    long long beforeBaseMs = 0;
    long long beforeAccumUs = 0;
    bool beforeActive = false;
    const bool beforeOk = ReadGameClockGlobals(beforeBaseMs, beforeAccumUs, beforeActive);
    const long long beforeClockMs = beforeOk
        ? beforeBaseMs + (beforeActive ? beforeAccumUs / 1000ll : 0ll)
        : 0ll;
    const unsigned long long beforeRealtimeMs = GetTickCount64();

    long long result = 0;
    if (g_pOrigGameTimeUpdate) {
        result = g_pOrigGameTimeUpdate(self, eventContext, timeContext, outTime);
    }

    long long afterBaseMs = 0;
    long long afterAccumUs = 0;
    bool afterActive = false;
    if (beforeOk && ReadGameClockGlobals(afterBaseMs, afterAccumUs, afterActive)) {
        long long afterClockMs = afterBaseMs + (afterActive ? afterAccumUs / 1000ll : 0ll);
        afterClockMs = ApplyRealGameTimeScale(beforeClockMs, afterClockMs, afterAccumUs, afterActive);
        StoreGameTimeProbeSample(beforeClockMs, beforeAccumUs, beforeRealtimeMs, afterClockMs, afterAccumUs, afterActive);
    }

    return result;
}

long long __fastcall Hooked_GameTimeGetter(unsigned char* source, long long outTime) {
    long long result = outTime;
    if (g_pOrigGameTimeGetter) {
        result = g_pOrigGameTimeGetter(source, outTime);
    }

    GameClockCalendar nativeCal;
    if (ReadGameClockCalendarStruct(outTime, nativeCal)) {
        if (ApplyRealGameTimeAction(source, outTime, nativeCal) ||
            ApplyPendingRealGameTimeFieldSync(source, outTime, nativeCal)) {
            ReadGameClockCalendarStruct(outTime, nativeCal);
        }
        ApplyRealGameTimeSnapshotGuard(source, outTime, nativeCal);
        SyncRealGameTimeFieldToGlobalClock(source, outTime);
        ApplyDevGameTimeOverride(source, outTime, nativeCal);
    }
    StoreGameTimeGetterSample(source, outTime);
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
        if (!envVt || !IsReadableTickPtr(reinterpret_cast<uintptr_t>(envVt), 0x48)) {
            return 0;
        }

        auto getRoot = reinterpret_cast<long long(__fastcall*)(long long)>(envVt[0x40 / 8]);
        const long long root = getRoot(envMgr);
        if (!root || !IsReadableTickPtr(static_cast<uintptr_t>(root + 0x1D0), sizeof(uintptr_t))) {
            return 0;
        }

        const long long audioProvider = *reinterpret_cast<long long*>(root + 0x1D0);
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
    if (!g_pAkPostEventById) {
        static DWORD64 s_lastUnavailableLog = 0;
        const DWORD64 now = GetTickCount64();
        if (now - s_lastUnavailableLog >= 10000) {
            s_lastUnavailableLog = now;
            Log("[thunder-audio] unavailable: AK::PostEventById missing\n");
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
        { 3685435772u, "env_oneshot_thunder" },
    };

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

static void TickNativeLightningBridge(long long self, float dt) {
    constexpr float kThunderSchedulerTickSeconds = 0.10f;

    if (!g_pNativeLightningScheduler || !g_pWeatherEffectGateByte || !self) {
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
    if (!IsReadableTickPtr(static_cast<uintptr_t>(self), 0xF0) ||
        !IsReadableTickPtr(reinterpret_cast<uintptr_t>(g_pWeatherEffectGateByte), sizeof(*g_pWeatherEffectGateByte))) {
        return;
    }

    uint8_t gate = 0;
    __try {
        gate = *g_pWeatherEffectGateByte;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (gate == 0) {
        return;
    }

    long long effect = 0;
    long long variation = 0;
    float elapsedBefore = 0.0f;
    float nextBefore = -1.0f;
    if (!TryReadTickValue(self, 0x78, effect) || !TryReadTickValue(self, 0x80, variation) ||
        !TryReadTickValue(self, 0xE4, elapsedBefore) || !TryReadTickValue(self, 0xE8, nextBefore)) {
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
        float& elapsed = At<float>(self, 0xE4);
        float& nextDelay = At<float>(self, 0xE8);
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
        g_thunderSchedulerRainBias.store(schedulerRain);
        g_pNativeLightningScheduler(self);
        g_thunderSchedulerRainBias.store(0.0f);
        if (std::isfinite(nextDelay) && nextDelay > maxDelay) {
            nextDelay = maxDelay;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_thunderSchedulerRainBias.store(0.0f);
        Log("[thunder] scheduler exception self=%p gate=%u\n", reinterpret_cast<void*>(self), gate);
        return;
    }

    float elapsedAfter = 0.0f;
    float nextAfter = -1.0f;
    TryReadTickValue(self, 0xE4, elapsedAfter);
    TryReadTickValue(self, 0xE8, nextAfter);

    const bool spawnedStrike = elapsedBefore > 0.5f && elapsedAfter < 0.1f && nextAfter < 0.0f;
    if (spawnedStrike) {
        TryPlayThunderAudio();
    }

    static DWORD64 s_lastLog = 0;
    const DWORD64 now = GetTickCount64();
    if (now - s_lastLog >= 10000) {
        s_lastLog = now;
        Log("[thunder] amount=%.3f rain=%.3f gate=%u e4=%.3f->%.3f e8=%.3f->%.3f effect=%p var=%p\n",
            thunder, rainHint, gate, elapsedBefore, elapsedAfter, nextBefore, nextAfter,
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

static bool WeatherTickCloudShapeWorkNeeded() {
    return g_oCloudSpdY.active.load();
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
        RainEffectCleanupActive() ||
        g_noRain.load() ||
        g_noSnow.load() ||
        g_noDust.load() ||
        g_oThunder.active.load() ||
        g_noWind.load() ||
        DustForcesCalmWind()) {
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
    UpdateRainEffectTransitionCleanup();

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
        StopWeatherEffectsByMask(self, 0x1DFu);
        TickRainEffectCleanup(self, env);
        ApplyClearWeatherParams(self, env);
        ApplyWindFromSlider(self, env);
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
    TickRainEffectCleanup(self, env);
    const uint32_t suppressedWeatherMask = ComputeSuppressedWeatherEffectMask();
    if (suppressedWeatherMask) {
        StopWeatherEffectsByMask(self, suppressedWeatherMask);
    }
    TickNativeLightningBridge(self, serviceDt);
    if (resetStopNow) {
        StopAllWeatherEffects(self);
    }
    if (env.valid && WeatherTickCloudShapeWorkNeeded()) {
        CaptureCloudBaseline(env);
        ApplyCloudOverrides(env);
    }
    ApplyDustWindPolicy(self, env);
    if (g_noWind.load()) {
        ApplyNoWindPolicy(self, env);
    }
    TickTimeControl();
}


