#include "pch.h"
#include "runtime_shared.h"
#include "preset_service.h"
#include <cstring>
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

// Intensity hooks feed slider overrides into the engine weather blend inputs.
__m128 __fastcall Hooked_GetRainIntensity(long long ws) {
    if (!g_modEnabled.load()) {
        return g_pOrigGetRainIntensity ? g_pOrigGetRainIntensity(ws) : PackScalar(0.0f);
    }
    if (g_forceClear.load()) return PackScalar(0.0f);
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
    if (g_forceClear.load()) return PackScalar(0.0f);
    if (g_oSnow.active.load())
        return PackScalar(g_oSnow.value.load());
    return g_pOrigGetSnowIntensity ? g_pOrigGetSnowIntensity(ws) : PackScalar(0.0f);
}

__m128 __fastcall Hooked_GetDustIntensity(long long ws) {
    if (!g_modEnabled.load()) {
        return g_pOrigGetDustIntensity ? g_pOrigGetDustIntensity(ws) : PackScalar(0.0f);
    }
    if (g_noWind.load())
        return PackScalar(0.0f);
    if (g_oDust.active.load()) {
        return PackScalar(DustSliderToNative(g_oDust.value.load()));
    }
    float v = g_pOrigGetDustIntensity ? ExtractScalar(g_pOrigGetDustIntensity(ws)) : 0.0f;
    float mul = g_windMul.load();
    if (mul < 0.0f) mul = 0.0f;
    if (mul > 15.0f) mul = 15.0f;
    return PackScalar(v * mul);
}

// Weather postprocess layer update hook.
void __fastcall Hooked_PPLayerUpdate(long long layerMgr, float dt) {
    if (g_pOrigPPLayerUpdate) g_pOrigPPLayerUpdate(layerMgr, dt);
}

static constexpr uint32_t kFogReceiverOverrideMask = 0x1F;
static constexpr float kFogOverdriveNormAt100 = 2.4f;
static constexpr float kFogDenseMax = 500.0f;
static constexpr bool kEnableDirectNodeWrites = true;
static constexpr bool kWeatherTickPassThroughOnly = false;

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

    if (g_forceClear.load()) {
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

static bool AnyCelestialOverrideActive() {
    return g_oSunDirX.active.load() || g_oSunDirY.active.load() ||
           g_oMoonDirX.active.load() || g_oMoonDirY.active.load();
}

static void CaptureAtmosphereCelestialBase(long long atmosphereObj) {
    if (!atmosphereObj) return;
    __try {
        g_atmoBaseSunDirX.store(At<float>(atmosphereObj, AC0::SUN_DIR_X));
        g_atmoBaseSunDirY.store(At<float>(atmosphereObj, AC0::SUN_DIR_Y));
        g_atmoBaseMoonDirX.store(At<float>(atmosphereObj, AC0::MOON_DIR_X));
        g_atmoBaseMoonDirY.store(At<float>(atmosphereObj, AC0::MOON_DIR_Y));
        g_atmoCelestialBaseValid.store(true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_atmoCelestialBaseValid.store(false);
    }
}

static void ApplyCelestialOverrides(const ResolvedEnv& env) {
    if (!env.atmosphereNode) {
        g_atmoCelestialBaseValid.store(false);
        return;
    }

    CaptureAtmosphereCelestialBase(env.atmosphereNode);
}

static float ResolveFogSetValue(int idx, float incoming) {
    if (!g_modEnabled.load()) return incoming;
    if (!g_oFog.active.load()) return incoming;
    if (idx < 0 || idx >= 5) return incoming;
    if ((kFogReceiverOverrideMask & (1u << idx)) == 0) return incoming;
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
    if (!self || !g_oFog.active.load() || !g_pOrigAtmosFogBlend) return;

    __try {
        struct FogOut {
            uint8_t pad[0x10];
            float v0, v1, v2, v3, v4;
        } out = {};

        g_pOrigAtmosFogBlend((long long)self, (long long)&out);

        float fog = max(0.0f, g_oFog.value.load());
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

void __fastcall Hooked_WindPack(long long* windNodePtr, float* packedOut) {
    if (g_pOrigWindPack) g_pOrigWindPack(windNodePtr, packedOut);
    if (!g_modEnabled.load()) return;
    if (!packedOut) return;
    if (g_forceClear.load()) return;

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
    if (!g_oExpNightSkyRot.active.load() && std::isfinite(packedOut[0x0A])) {
        g_windPackBase0A.store(packedOut[0x0A]);
        g_windPackBase0AValid.store(true);
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
    const bool exp0AActive = g_oExpNightSkyRot.active.load() && g_windPackBase0AValid.load();
    if (exp2CActive) {
        const float mul = min(15.0f, max(0.0f, g_oExpCloud2C.get(1.0f)));
        packedOut[0x2C] = g_windPackBase2C.load() * mul;
    }
    if (exp2DActive) {
        const float mul = min(15.0f, max(0.0f, g_oExpCloud2D.get(1.0f)));
        packedOut[0x2D] = g_windPackBase2D.load() * mul;
    }
    if (exp0AActive) {
        const float value = min(15.0f, max(-15.0f, g_oExpNightSkyRot.get(1.0f)));
        packedOut[0x0A] = g_windPackBase0A.load() * value;
    }

    if (g_oCloudVariation.active.load() && g_windPackBase32Valid.load()) {
        const float mul = min(15.0f, max(0.0f, g_oCloudVariation.get(1.0f)));
        packedOut[0x32] = g_windPackBase32.load() * mul;
    }

    if (g_oNativeFog.active.load()) {
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
    PrimeFogSetHooksFromFrame(self);

    if (self && g_oFog.active.load()) {
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

        if (g_oDust.active.load()) {
            At<float>(env.cloudNode, CN::DUST_BASE) = nativeDust;
            At<float>(env.cloudNode, CN::DUST_ADD) = nativeDust * 0.10f;
            At<float>(env.cloudNode, CN::DUST_WIND_SCALE) = max(0.30f, dust * 0.60f);
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
    float rain = g_oRain.active.load() ? g_oRain.value.load() : 0.0f;
    float snow = g_oSnow.active.load() ? g_oSnow.value.load() : 0.0f;
    float dust = g_oDust.active.load() ? g_oDust.value.load() : 0.0f;
    float wind = g_oWindActual.active.load() ? g_oWindActual.value.load() : 0.0f;

    if (rain > 0.01f) mask |= 0x003;      // effects 0,1 (rain drops)
    if (rain > 0.5f)  mask |= 0x010;      // effect 4 (heavy rain)
    if (snow > 0.01f) mask |= 0x004;      // effect 2 (snow flakes)
    if (snow > 0.3f)  mask |= 0x008;      // effect 3 (heavy snow)
    if (wind > 0.5f)  mask |= 0x020;      // effect 5 (wind base)
    if (dust > 0.1f)  mask |= 0x040;      // effect 6 (sand dust)

    return mask;
}

static bool IsRainOnlyControlMode() {
    bool rainDriven = g_oRain.active.load();
    bool others = g_oSnow.active.load() || g_oDust.active.load() ||
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
    float rain = g_oRain.active.load() ? g_oRain.value.load() : 0.0f;

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

static void ApplyNoWindPolicy(long long self, const ResolvedEnv& env) {
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
        ApplyCelestialOverrides(env);
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
                    if (i <= 1) effI = g_oRain.active.load() ? g_oRain.value.load() : 0.0f;
                    else if (i == 2 || i == 3) effI = g_oSnow.active.load() ? g_oSnow.value.load() : 0.0f;
                    else if (i == 4) effI = g_oRain.active.load() ? max(0.f, g_oRain.value.load() - 0.5f) * 2.f : 0.0f;
                    else if (i == 5) effI = g_oWindActual.active.load() ? min(1.f, g_oWindActual.value.load() / 10.f) : 0.0f;
                    else if (i == 6) effI = g_oDust.active.load() ? min(1.0f, g_oDust.value.load()) : 0.0f;
                    else if (i == 7) effI = min(
                        g_oSnow.active.load() ? g_oSnow.value.load() : 0.0f,
                        g_oDust.active.load() ? g_oDust.value.load() : 0.0f);
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

// Hooked weather tick; keep the current stable call order.
void __fastcall Hooked_WeatherTick(long long self, float dt) {
    if (kWeatherTickPassThroughOnly) {
        if (g_pOriginalTick) g_pOriginalTick(self, dt);
        return;
    }

    const bool resetStopNow = g_resetStopRequested.exchange(false);
    const bool modSuspendNow = g_modSuspendRequested.exchange(false);
    const ResolvedEnv env = ResolveEnv();
    const bool worldReady = env.valid && env.cloudNode && env.windNode && env.particleMgr;
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
        ApplyCelestialOverrides(env);
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

    const bool rainOnlyControl = IsRainOnlyControlMode();
    const bool dustDriven = g_oDust.active.load();
    if (!dustDriven) {
        TickWeatherState(self, dt);
    }
    g_pOriginalTick(self, dt);
    if (dustDriven && g_activeWeather == kCustomWeather) {
        TickWeatherState(self, dt);
    }
    if (rainOnlyControl && g_activeWeather == kCustomWeather) {
        if (env.valid) {
            const int nullSent = g_pNullSentinel ? *g_pNullSentinel : 0;
            TickRainOnly(self, env, nullSent);
        }
    }
    if (resetStopNow) {
        StopAllWeatherEffects(self);
    }
    if (kEnableDirectNodeWrites && env.valid) {
        CaptureCloudBaseline(env);
        ApplyCloudOverrides(env);
        ApplyCelestialOverrides(env);
    }
    if (g_noWind.load()) {
        ApplyNoWindPolicy(self, env);
    }
    TickTimeControl();
}


