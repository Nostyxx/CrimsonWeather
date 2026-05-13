#include "pch.h"
#include "runtime_shared.h"
#include "preset_service.h"
#include <cstdarg>
#include <cfloat>
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

        r.atmosphereNode = r.windNode;
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

struct RegionAnchor {
    int localId;
    int majorId;
    float x;
    float y;
    float z;
};

struct RegionClassification {
    int majorId = 0;
    int localId = 0;
};

static RegionClassification ClassifyRegionFromPosition(float x, float y, float z) {
    if (y > 1400.0f) {
        return { 6, 0 }; // Abyss
    }

    // Bias the Hernand -> Demeniss -> Delesyia corridor to match the game's border toasts.
    // Pure nearest-anchor classification switches to Demeniss noticeably after
    // the game's own border toast, so bias this corridor westward.
    if (z >= -4700.0f && z <= -2500.0f) {
        if (x <= -9600.0f) {
            return { 1, 1 }; // Hernand
        }
        if (x <= -6500.0f) {
            return { 2, 2 }; // Demeniss
        }
        if (x <= -4200.0f && z <= -3600.0f) {
            return { 3, 3 }; // Delesyia
        }
    }

    // Position anchors captured from the main regional route:
    // Hernand -> Demeniss -> Delesyia -> Tashkalp -> Urdavah -> Pailune -> Varnia.
    static constexpr RegionAnchor kAnchors[] = {
        { 1, 1, -10479.6f, 553.0f, -4158.2f }, // Hernand
        { 2, 2,  -7721.1f, 522.1f, -3263.9f }, // Demeniss
        { 3, 3,  -5597.5f, 520.6f, -4275.2f }, // Delesyia
        { 4, 5,  -6111.7f, 616.9f,  -888.0f }, // Tashkalp -> Crimson Desert
        { 5, 5,  -6489.3f, 972.7f,   103.0f }, // Urdavah -> Crimson Desert
        { 6, 4, -10612.6f, 957.1f,   123.5f }, // Pailune
        { 7, 5,  -3836.8f, 695.1f,  3529.6f }, // Varnia -> Crimson Desert
    };

    RegionClassification result{};
    float bestDistSq = FLT_MAX;
    for (const RegionAnchor& anchor : kAnchors) {
        const float dx = x - anchor.x;
        const float dz = z - anchor.z;
        const float distSq = dx * dx + dz * dz;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            result.majorId = anchor.majorId;
            result.localId = anchor.localId;
        }
    }
    return result;
}

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

static RegionClassification UpdateStableGameHudRegion(float x, float y, float z) {
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

    RegionClassification classified = UpdateStableGameHudRegion(x, y, z);

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
static constexpr bool kEnableDirectNodeWrites = true;

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

void __fastcall Hooked_AtmosFogBlend(long long ctx, long long outParams) {
    if (g_pOrigAtmosFogBlend) g_pOrigAtmosFogBlend(ctx, outParams);
    if (!g_modEnabled.load()) return;
    if (!outParams) return;

    if (g_forceClear.load() || g_noFog.load()) {
        __try {
            float* p = reinterpret_cast<float*>(outParams + 0x10);
            p[0] = 0.0f; p[1] = 0.0f; p[2] = 0.0f; p[3] = 0.0f; p[4] = 2.0f;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }

    if (!g_oFog.active.load()) return;

    __try {
        float* p = reinterpret_cast<float*>(outParams + 0x10);
        float profile[5] = {};
        float fog = max(0.0f, g_oFog.value.load());
        ApplyAuthoritativeFogProfile(fog, profile[0], profile[1], profile[2], profile[3], profile[4]);
        for (int i = 0; i < 5; ++i) {
            if ((kFogReceiverOverrideMask & (1u << i)) != 0) {
                p[i] = profile[i];
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[W] fog override exception in AtmosFogBlend\n");
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

static void PrimeFogSetHooksFromFrame(long long* self) {
    if (!g_pOrigAtmosFogBlend) return;
    if (!self || g_fogSetHooksInstalled.load() || g_fogSetHooksAttempted.load()) return;
    __try {
        long long provider = *reinterpret_cast<long long*>(reinterpret_cast<uint8_t*>(self) + 0x48);
        if (!provider) return;
        auto pVt = *reinterpret_cast<uintptr_t**>(provider);
        if (!pVt) return;
        auto getReceiver = reinterpret_cast<FogReceiverGetter_fn>(pVt[0x190 / 8]);
        if (!getReceiver) return;
        long long* receiver = getReceiver(provider);
        if (!receiver) return;
        auto rVt = *reinterpret_cast<uintptr_t**>(receiver);
        if (!rVt) return;
        TryInstallFogSetHooks(rVt);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void ForceApplyFogFromFrame(long long* self) {
    const bool forceClear = g_forceClear.load();
    const bool noFog = g_noFog.load();
    if (!self || (!forceClear && !noFog && !g_oFog.active.load()) || !g_pOrigAtmosFogBlend) return;

    __try {
        struct FogOut {
            uint8_t pad[0x10];
            float v0, v1, v2, v3, v4;
        } out = {};

        g_pOrigAtmosFogBlend((long long)self, (long long)&out);

        float fog = (forceClear || noFog) ? 0.0f : max(0.0f, g_oFog.value.load());
        ApplyAuthoritativeFogProfile(fog, out.v0, out.v1, out.v2, out.v3, out.v4);

        long long provider = *reinterpret_cast<long long*>(reinterpret_cast<uint8_t*>(self) + 0x48);
        if (!provider) return;
        auto pVt = *reinterpret_cast<uintptr_t**>(provider);
        if (!pVt) return;
        auto getReceiver = reinterpret_cast<FogReceiverGetter_fn>(pVt[0x190 / 8]);
        if (!getReceiver) return;
        long long* receiver = getReceiver(provider);
        if (!receiver) return;
        auto rVt = *reinterpret_cast<uintptr_t**>(receiver);
        if (!rVt) return;

        TryInstallFogSetHooks(rVt);
        g_forcedFogSet[0].store(out.v0);
        g_forcedFogSet[1].store(out.v1);
        g_forcedFogSet[2].store(out.v2);
        g_forcedFogSet[3].store(out.v3);
        g_forcedFogSet[4].store(out.v4);

        auto set0 = reinterpret_cast<FogReceiverSet_fn>(rVt[0x08 / 8]);
        auto set1 = reinterpret_cast<FogReceiverSet_fn>(rVt[0x10 / 8]);
        auto set2 = reinterpret_cast<FogReceiverSet_fn>(rVt[0x18 / 8]);
        auto set3 = reinterpret_cast<FogReceiverSet_fn>(rVt[0x28 / 8]);
        auto set4 = reinterpret_cast<FogReceiverSet_fn>(rVt[0x20 / 8]);
        if ((kFogReceiverOverrideMask & (1u << 0)) != 0 && set0) set0(receiver, out.v0);
        if ((kFogReceiverOverrideMask & (1u << 1)) != 0 && set1) set1(receiver, out.v1);
        if ((kFogReceiverOverrideMask & (1u << 2)) != 0 && set2) set2(receiver, out.v2);
        if ((kFogReceiverOverrideMask & (1u << 3)) != 0 && set3) set3(receiver, out.v3);
        if ((kFogReceiverOverrideMask & (1u << 4)) != 0 && set4) set4(receiver, out.v4);

    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static void RememberGameGlobalEffectManager(long long* self) {
    if (!self || !IsReadableTickPtr(reinterpret_cast<uintptr_t>(self), 9 * sizeof(long long))) {
        return;
    }

    __try {
        const uintptr_t manager = static_cast<uintptr_t>(self[8]);
        if (manager && IsReadableTickPtr(manager, 0x100)) {
            g_lastGameGlobalEffectManager.store(manager);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

static unsigned short SelectThunderGlobalEffectId(float rainHint) {
    constexpr unsigned short kRainLightning = 0x4244;
    constexpr unsigned short kHeavyRainLightning = 0x42DC;
    constexpr unsigned short kLightningDryWeather = 0x428E;

    if (rainHint >= 0.75f) {
        return kHeavyRainLightning;
    }
    if (rainHint >= 0.10f) {
        return kRainLightning;
    }
    return kLightningDryWeather;
}

static const char* ThunderGlobalEffectName(unsigned short id) {
    switch (id) {
    case 0x4244: return "RainLightning";
    case 0x42DC: return "HeavyRainLightning";
    case 0x42C6: return "LightningDry";
    case 0x428E: return "LightningDryWeather";
    default: return "Unknown";
    }
}

static float ThunderRateCurve(float thunder) {
    thunder = min(1.0f, max(0.0f, thunder));
    return powf(thunder, 0.55f);
}

static int ThunderVisualClusterCount(float thunder) {
    if (thunder >= 0.98f) return 4;
    if (thunder >= 0.90f) return 3;
    if (thunder >= 0.75f) return 2;
    return 1;
}

static void TriggerThunderGlobalEffect(float rainHint, float thunder) {
    if (!g_pSpawnGameGlobalEffect) {
        return;
    }

    const uintptr_t manager = g_lastGameGlobalEffectManager.load();
    if (!manager || !IsReadableTickPtr(manager, 0x100)) {
        return;
    }

    static DWORD64 s_lastNativeGlobalEffectMs = 0;
    const DWORD64 now = GetTickCount64();
    const float rate = ThunderRateCurve(thunder);
    const DWORD64 minGapMs = static_cast<DWORD64>((0.25f + (1.0f - rate) * 6.0f) * 1000.0f);
    if (now - s_lastNativeGlobalEffectMs < minGapMs) {
        return;
    }
    s_lastNativeGlobalEffectMs = now;

    unsigned short effectId = SelectThunderGlobalEffectId(rainHint);
    const int clusterCount = ThunderVisualClusterCount(thunder);
    long long lastResult = 0;
    int spawned = 0;
    for (int i = 0; i < clusterCount; ++i) {
        unsigned short spawnId = effectId;
        __try {
            lastResult = g_pSpawnGameGlobalEffect(static_cast<long long>(manager), &spawnId);
            ++spawned;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[thunder-native] exception manager=%p effect=%s id=0x%04X cluster=%d/%d\n",
                reinterpret_cast<void*>(manager), ThunderGlobalEffectName(spawnId), spawnId, i + 1, clusterCount);
            break;
        }
    }

    Log("[thunder-native] spawn manager=%p effect=%s id=0x%04X result=%lld rain=%.3f amount=%.3f cluster=%d/%d gap=%llu\n",
        reinterpret_cast<void*>(manager), ThunderGlobalEffectName(effectId), effectId,
        lastResult, rainHint, thunder, spawned, clusterCount,
        static_cast<unsigned long long>(minGapMs));
}

static constexpr float kCloudGeometryMax = 64.0f;

struct CloudGeometry {
    float top;
    float thick;
    float base;
    float shapeA;
    float shapeB;
    float shapeC;
};

static void ClampCloudGeometry(CloudGeometry& g) {
    g.top = max(0.0f, g.top);
    g.thick = max(0.0f, g.thick);
    g.base = max(0.0f, g.base);
    g.shapeA = max(0.0f, g.shapeA);
    g.shapeB = max(0.0f, g.shapeB);
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
    out = { top, thick, base, sA, 0.0f, sC };
    ClampCloudGeometry(out);
    return IsReasonableCloudGeometry(out);
}

static bool LoadStoredCloudBase(CloudGeometry& out) {
    if (!g_cloudBaseValid.load()) return false;
    out.top = max(0.0f, g_cloudBaseTop.load());
    out.thick = max(0.0f, g_cloudBaseThick.load());
    out.base = max(0.0f, g_cloudBaseBase.load());
    out.shapeA = max(0.0f, g_cloudBaseShapeA.load());
    out.shapeB = 0.0f;
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
            g_cloudBaseShapeB.store(live.shapeB);
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
    if (!AnyPackedCelestialOverrideActive() || !g_atmoCelestialBaseValid.load()) return;

    if (g_oSunSize.active.load()) {
        const float size = ClampFloat(g_oSunSize.value.load(), 0.01f, 10.0f);
        packedOut[kPackedSunSize] = size;
        packedOut[kPackedSunSizeCos] = cosf(size * kPackedAngleToRad);
    }
    if (g_oMoonSize.active.load()) {
        const float size = ClampFloat(g_oMoonSize.value.load(), 0.020f, 20.0f);
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
        if (!sceneOwner) return result;
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
    if (!modEnabled) return;
    if (!packedOut) return;

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
    if (!g_oNativeFog.active.load() && std::isfinite(packedOut[0x11])) {
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

}

void __fastcall Hooked_WeatherFrameUpdate(long long* self, float dt) {
    if (!g_modEnabled.load()) {
        if (g_pOrigWeatherFrameUpdate) g_pOrigWeatherFrameUpdate(self, dt);
        return;
    }
    RememberGameGlobalEffectManager(self);
    PrimeFogSetHooksFromFrame(self);

    if (self && (g_forceClear.load() || g_noFog.load() || g_oFog.active.load())) {
        __try {
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0x98) = 0.0f;
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0x9C) = 0.0f;
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0xA0) = 0.0f;
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0xA4) = 0.0f;
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0xA8) = 0.0f;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    if (g_pOrigWeatherFrameUpdate) g_pOrigWeatherFrameUpdate(self, dt);

    RememberGameGlobalEffectManager(self);
    ForceApplyFogFromFrame(self);

    (void)dt;
}

// Clear weather parameters.
static void ApplyClearWeatherParams(long long self, const ResolvedEnv& env) {
    if (!env.valid) return;
    if (kEnableDirectNodeWrites && env.cloudNode) {
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
    if (kEnableDirectNodeWrites && env.cloudNode) {
        float fogTotal = g_oFog.get(0.0f);
        float dust = g_oDust.active.load() ? g_oDust.value.load() : 0.0f;
        float nativeDust = DustSliderToNative(dust);
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

        if (g_noDust.load()) {
            At<float>(env.cloudNode, CN::DUST_BASE) = 0.0f;
            At<float>(env.cloudNode, CN::DUST_ADD) = 0.0f;
            At<float>(env.cloudNode, CN::DUST_WIND_SCALE) = 0.0f;
            At<float>(env.cloudNode, CN::DUST_THRESH) = 0.0f;
        }

        if (!g_noDust.load() && g_oDust.active.load()) {
            At<float>(env.cloudNode, CN::DUST_BASE) = nativeDust;
            At<float>(env.cloudNode, CN::DUST_ADD) = nativeDust * 0.10f;
            At<float>(env.cloudNode, CN::DUST_WIND_SCALE) = DustSliderToWindScale(dust);
            At<float>(env.cloudNode, CN::DUST_THRESH) = min(At<float>(env.cloudNode, CN::DUST_THRESH), DustSliderToThreshold(dust));
            At<float>(env.cloudNode, CN::STORM_THRESH) = max(At<float>(env.cloudNode, CN::STORM_THRESH), DustSliderToStorm(dust));
            At<float>(env.cloudNode, CN::FOG_B) = max(At<float>(env.cloudNode, CN::FOG_B), DustSliderToFogB(dust));
        }

    }

    if (kEnableDirectNodeWrites) {
        ApplyCloudOverrides(env);
    }

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

    if (kEnableDirectNodeWrites && env.cloudNode) {
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
    if (kEnableDirectNodeWrites && env.windNode) {
        At<float>(env.windNode, WN::SPEED) = 0.0f;
        At<float>(env.windNode, WN::GUST)  = 0.0f;
    }
}

static void ApplyDustWindPolicy(long long self, const ResolvedEnv& env) {
    if (!env.valid || !DustForcesCalmWind()) return;
    At<int>(self, WCO::SOUND_WIND)    = 0;
    At<int>(self, WCO::SOUND_SKYWIND) = 0;
    if (kEnableDirectNodeWrites && env.windNode) {
        At<float>(env.windNode, WN::SPEED) = 0.0f;
        At<float>(env.windNode, WN::GUST)  = 0.0f;
    }
}

// Process wind state hook.
void __fastcall Hooked_ProcessWindState(long long self) {
    if (g_pOrigProcessWindState) g_pOrigProcessWindState(self);
    if (!g_modEnabled.load()) return;
    const ResolvedEnv env = ResolveEnv();
    if (kEnableDirectNodeWrites && env.valid) {
        CaptureCloudBaseline(env);
        ApplyCloudOverrides(env);
    }
}

static void ApplyWindFromSlider(long long self, const ResolvedEnv& env) {
    if (!env.valid) return;
    if (!g_oWindActual.active.load()) return;

    float wSpd = max(0.0f, g_oWindActual.value.load());
    if (kEnableDirectNodeWrites && env.windNode) {
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

static void TryPlayThunderAudio(float thunder) {
    (void)thunder;
    if (!g_pAkPostEventById) {
        return;
    }

    EnsureThunderSoundBanksLoaded();

    static DWORD64 s_lastThunderSound = 0;
    const DWORD64 now = GetTickCount64();
    if (now - s_lastThunderSound < 500) {
        return;
    }
    s_lastThunderSound = now;

    constexpr ThunderAudioCandidate kThunderAudioCandidates[] = {
        { 3685435772u, "env_oneshot_thunder" },
    };

    const uint64_t gameObjectId = ResolveWeatherAudioGameObjectId();
    for (const ThunderAudioCandidate& candidate : kThunderAudioCandidates) {
        uint32_t playingId = 0;
        __try {
            playingId = g_pAkPostEventById(candidate.eventId, gameObjectId, 0, nullptr, nullptr, 0, nullptr, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[thunder-audio] post exception name=%s id=%u object=%llu\n",
                candidate.eventName, candidate.eventId, static_cast<unsigned long long>(gameObjectId));
            continue;
        }
        Log("[thunder-audio] post event=%s id=%u object=%llu playing=%u\n",
            candidate.eventName, candidate.eventId,
            static_cast<unsigned long long>(gameObjectId), playingId);
        if (playingId) {
            break;
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
        TriggerThunderGlobalEffect(rainHint, thunder);
        TryPlayThunderAudio(thunder);
    }

    static DWORD64 s_lastLog = 0;
    const DWORD64 now = GetTickCount64();
    if (now - s_lastLog >= 2000) {
        s_lastLog = now;
        Log("[thunder] amount=%.3f rain=%.3f gate=%u e4=%.3f->%.3f e8=%.3f->%.3f effect=%p var=%p\n",
            thunder, rainHint, gate, elapsedBefore, elapsedAfter, nextBefore, nextAfter,
            reinterpret_cast<void*>(effect), reinterpret_cast<void*>(variation));
    }
}

// Hooked weather tick.
void __fastcall Hooked_WeatherTick(long long self, float dt) {
    const bool resetStopNow = g_resetStopRequested.exchange(false);
    const bool modSuspendNow = g_modSuspendRequested.exchange(false);
    const ResolvedEnv env = ResolveEnv();
    const bool worldReady = env.entity && env.weatherState;
    UpdateRegionState(env, dt);
    Preset_OnWorldTick(worldReady, dt);

    if (!g_modEnabled.load()) {
        g_pOriginalTick(self, dt);
        if (modSuspendNow || resetStopNow) {
            StopAllWeatherEffects(self);
            g_activeWeather = -1;
        }
        SuspendTimeControl();
        return;
    }

    if (g_forceClear.load()) {
        g_pOriginalTick(self, dt);
        StopWeatherEffectsByMask(self, 0x1DFu); 
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
    if (g_activeWeather == kCustomWeather) {
        TickWeatherState(self, dt);
    }
    const uint32_t suppressedWeatherMask = ComputeSuppressedWeatherEffectMask();
    if (suppressedWeatherMask) {
        StopWeatherEffectsByMask(self, suppressedWeatherMask);
    }
    TickNativeLightningBridge(self, dt);
    if (resetStopNow) {
        StopAllWeatherEffects(self);
    }
    if (kEnableDirectNodeWrites && env.valid) {
        CaptureCloudBaseline(env);
        ApplyCloudOverrides(env);
    }
    ApplyDustWindPolicy(self, env);
    if (g_noWind.load()) {
        ApplyNoWindPolicy(self, env);
    }
    TickTimeControl();
}


