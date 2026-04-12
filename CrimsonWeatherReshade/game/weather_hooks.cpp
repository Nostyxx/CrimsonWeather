#include "pch.h"
#include "runtime_shared.h"
#include "preset_service.h"
#include <cstring>
#include <iterator>

// Intensity hooks feed slider overrides into the engine weather blend inputs.
float __fastcall Hooked_GetRainIntensity(long long ws) {
    if (!g_modEnabled.load()) {
        return g_pOrigGetRainIntensity ? g_pOrigGetRainIntensity(ws) : 0.0f;
    }
    if (g_forceClear.load()) return 0.0f;
    if (g_oRain.active.load())
        return g_oRain.value.load();

    float baseRain = g_pOrigGetRainIntensity ? g_pOrigGetRainIntensity(ws) : 0.0f;
    if (baseRain > 0.01f) {
        static ULONGLONG s_lastRainBaseLog = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_lastRainBaseLog > 1500) {
            s_lastRainBaseLog = now;
            Log("[rain] base weather rain=%.3f (override inactive)\n", baseRain);
        }
    }
    return baseRain;
}

float __fastcall Hooked_GetSnowIntensity(long long ws) {
    if (!g_modEnabled.load()) {
        return g_pOrigGetSnowIntensity ? g_pOrigGetSnowIntensity(ws) : 0.0f;
    }
    if (g_forceClear.load()) return 0.0f;
    if (g_oSnow.active.load())
        return g_oSnow.value.load();
    return g_pOrigGetSnowIntensity ? g_pOrigGetSnowIntensity(ws) : 0.0f;
}

float __fastcall Hooked_GetDustIntensity(long long ws, unsigned int p2) {
    if (!g_modEnabled.load()) {
        return g_pOrigGetDustIntensity ? g_pOrigGetDustIntensity(ws, p2) : 0.0f;
    }
    if (g_noWind.load())
        return 0.0f;
    float v = 0.0f;
    if (g_oDust.active.load()) {
        v = g_oDust.value.load();
    } else {
        v = g_pOrigGetDustIntensity ? g_pOrigGetDustIntensity(ws, p2) : 0.0f;
    }
    float mul = g_windMul.load();
    if (mul < 0.0f) mul = 0.0f;
    if (mul > 5.0f) mul = 5.0f;
    return v * mul;
}

// Weather postprocess layer update hook.
void __fastcall Hooked_PPLayerUpdate(long long layerMgr, float dt) {
    if (g_pOrigPPLayerUpdate) g_pOrigPPLayerUpdate(layerMgr, dt);
}

static constexpr bool kEnableFogReceiverForce = true;
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

    if (!kEnableFogReceiverForce) {
        if (g_oFog.active.load()) {
            __try {
                float* p = reinterpret_cast<float*>(outParams + 0x10);
                g_fogPipeLastOut[0].store(p[0]);
                g_fogPipeLastOut[1].store(p[1]);
                g_fogPipeLastOut[2].store(p[2]);
                g_fogPipeLastOut[3].store(p[3]);
                g_fogPipeLastOut[4].store(p[4]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("[W] fog pass-through sampling exception\n");
            }
        }
        return;
    }
    if (!g_oFog.active.load()) return;

    __try {
        float* p = reinterpret_cast<float*>(outParams + 0x10);
        const float in0 = p[0], in1 = p[1], in2 = p[2], in3 = p[3], in4 = p[4];
        float fog = max(0.0f, g_oFog.value.load());
        ApplyAuthoritativeFogProfile(fog, p[0], p[1], p[2], p[3], p[4]);

        g_fogPipeLastOut[0].store(p[0]);
        g_fogPipeLastOut[1].store(p[1]);
        g_fogPipeLastOut[2].store(p[2]);
        g_fogPipeLastOut[3].store(p[3]);
        g_fogPipeLastOut[4].store(p[4]);
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

__int64 __fastcall Hooked_AtmosphereConstSummary(
    __int64 a1,
    __int64 a2,
    __int64 atmosphereObj,
    __int64 a4,
    __int64 a5) {
    if (!g_pOrigAtmosphereConstSummary) {
        return 0;
    }
    return g_pOrigAtmosphereConstSummary(a1, a2, atmosphereObj, a4, a5);
}

static float ResolveFogSetValue(int idx, float incoming) {
    if (!g_modEnabled.load()) return incoming;
    if (!g_oFog.active.load()) return incoming;
    if (idx < 0 || idx >= 5) return incoming;
    float forced = g_forcedFogSet[idx].load();
    return std::isfinite(forced) ? forced : incoming;
}

static void TrackFogSetCall(int idx, float inV, float outV) {
    if (idx < 0 || idx >= 5) return;
    g_fogSetLastIn[idx].store(inV);
    g_fogSetLastOut[idx].store(outV);
    g_fogSetCount[idx].fetch_add(1);
}

template <int Index>
static void __fastcall Hooked_FogSetImpl(long long* receiver, float value) {
    float outV = ResolveFogSetValue(Index, value);
    auto fn = g_pOrigFogSet[Index];
    if (fn) fn(receiver, outV);
    TrackFogSetCall(Index, value, outV);
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
            Log("[W] fogset[%d] vtbl+0x%X is null\n", h.idx, h.vtOff);
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
            Log("[W] fogset[%d] duplicate target %p, skip extra hook\n", h.idx, (void*)addr);
            continue;
        }

        bool ok = InstallHook((void*)addr, h.detour, (void**)&g_pOrigFogSet[h.idx], h.name, false);
        if (ok && g_pOrigFogSet[h.idx]) anyInstalled = true;
    }

    g_fogSetHooksInstalled.store(anyInstalled);
    Log("[fogset] install done installed=%d addrs=(%p,%p,%p,%p,%p)\n",
        anyInstalled ? 1 : 0,
        (void*)g_addrFogSet[0], (void*)g_addrFogSet[1], (void*)g_addrFogSet[2],
        (void*)g_addrFogSet[3], (void*)g_addrFogSet[4]);
}

static void PrimeFogSetHooksFromFrame(long long* self) {
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
        Log("[fogset] exception while prewarming fog setter hooks\n");
    }
}

static void ForceApplyFogFromFrame(long long* self) {
    if (!kEnableFogReceiverForce) return;
    if (!self || !g_oFog.active.load() || !g_pOrigAtmosFogBlend) return;

    __try {
        struct FogOut {
            uint8_t pad[0x10];
            float v0, v1, v2, v3, v4;
        } out = {};

        // Always sample the engine's own fog blend first.
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
        if (set0) set0(receiver, out.v0);
        if (set1) set1(receiver, out.v1);
        if (set2) set2(receiver, out.v2);
        if (set3) set3(receiver, out.v3);
        if (set4) set4(receiver, out.v4);

    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[W] fog force-apply exception\n");
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

static bool IsMeaningfulCloudGeometry(const CloudGeometry& g) {
    return g.top > (g.base + 0.05f) ||
        max(g.shapeA, g.shapeC) > 0.05f;
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
    const bool cloudShapeOverrideActive = g_oCloudSpdX.active.load() || g_oCloudSpdY.active.load();
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
    const bool cloudShapeActive = g_oCloudSpdX.active.load() || g_oCloudSpdY.active.load();
    if (!cloudShapeActive) return;

    const float mulX = cloudShapeActive ? CloudXToSafeMul(g_oCloudSpdX.get(1.0f)) : 1.0f;
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

static AtmosphereCloudPack GetAuthoredCloudBasePack() {
    return { 0.0f, 0.0f, 0.0f, 0.0f, 1.80f, 0.0f, 0.0f, 0.28f, 0.0f };
}

static void LogForceCloudsAtmosphere(const AtmosphereCloudPack& nativePack,
                                     const AtmosphereCloudPack& seedPack,
                                     const AtmosphereCloudPack& finalPack) {
    if (!g_forceCloudsEnabled.load()) return;
    static ULONGLONG s_lastForceCloudAtmoLog = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastForceCloudAtmoLog <= 1000) return;
    s_lastForceCloudAtmoLog = now;

    Log("[cloud-base-atmo] amt=%.2f native=(scale=%.3f vis=%.1f dens=%.3f alpha=%.3f thick=%.3f near=%.3f) "
        "seed=(alpha=%.3f thick=%.3f) final=(scale=%.3f vis=%.1f dens=%.3f alpha=%.3f thick=%.3f near=%.3f)\n",
        min(1.0f, max(0.0f, g_forceCloudsAmount.load())),
        nativePack.baseScale, nativePack.visibleRange, nativePack.density, nativePack.alpha,
        nativePack.thickness, nativePack.nearPlane,
        seedPack.alpha, seedPack.thickness,
        finalPack.baseScale, finalPack.visibleRange, finalPack.density, finalPack.alpha,
        finalPack.thickness, finalPack.nearPlane);
}
float __fastcall Hooked_WindPack(long long* windNodePtr, float* packedOut) {
    float ret = g_pOrigWindPack ? g_pOrigWindPack(windNodePtr, packedOut) : 0.0f;
    if (!g_modEnabled.load()) return ret;
    if (!packedOut) return ret;
    if (g_forceClear.load()) return ret;

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

    const bool cloudActive = g_oCloudSpdX.active.load() || g_oCloudSpdY.active.load();
    if (cloudActive && g_windPackBaseValid.load()) {
        float mulX = CloudXToSafeMul(g_oCloudSpdX.get(1.0f));
        float mulZ = min(10.0f, max(0.0f, g_oCloudSpdY.get(1.0f)));

        packedOut[0x23] = g_windPackBase23.load() * mulX;
        packedOut[0x24] = g_windPackBase24.load() * mulZ;
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

    return ret;
}

void __fastcall Hooked_CloudPack(long long self, long long* cloudNodePtr, float* packedOut,
                                 unsigned long long* p4, unsigned long long* p5, char p6,
                                 float driftX, float driftZ) {
    // TODO: CloudPack override not yet implemented. Pass-through only.
    if (g_pOrigCloudPack) {
        g_pOrigCloudPack(self, cloudNodePtr, packedOut, p4, p5, p6, driftX, driftZ);
    }
}

void __fastcall Hooked_WeatherFrameUpdate(long long* self, float dt) {
    if (!g_modEnabled.load()) {
        if (g_pOrigWeatherFrameUpdate) g_pOrigWeatherFrameUpdate(self, dt);
        return;
    }
    PrimeFogSetHooksFromFrame(self);

        if (kEnableFogReceiverForce && self && g_oFog.active.load()) {
        __try {
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0x98) = 0.0f;
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0x9C) = 0.0f;
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0xA0) = 0.0f;
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0xA4) = 0.0f;
            *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(self) + 0xA8) = 0.0f;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[W] fog frame blend-force exception\n");
        }
    }

    if (g_pOrigWeatherFrameUpdate) g_pOrigWeatherFrameUpdate(self, dt);

    ForceApplyFogFromFrame(self);

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

    }

    // WindNode: speed.
    if (env.windNode && g_oWind.active.load()) {
        float wSpd = max(0.0f, g_oWind.value.load());
        At<float>(env.windNode, WN::SPEED) = wSpd;
    }
    ApplyCloudOverrides(env);

    // WeatherComponent lerp overrides.
    At<float>(self, WCO::LERP_ALPHA)     = 1.0f;
    At<float>(self, WCO::BLEND_DIR_MULT) = 1.0f;
}
static uint32_t ComputeCustomEffectMask() {
    uint32_t mask = 0;
    float rain = g_oRain.active.load() ? g_oRain.value.load() : 0.0f;
    float snow = g_oSnow.active.load() ? g_oSnow.value.load() : 0.0f;
    float dust = g_oDust.active.load() ? g_oDust.value.load() : 0.0f;
    float wind = g_oWind.active.load() ? g_oWind.value.load() : 0.0f;

    if (rain > 0.01f) mask |= 0x003;      // effects 0,1 (rain drops)
    if (rain > 0.5f)  mask |= 0x010;      // effect 4 (heavy rain)
    if (snow > 0.01f) mask |= 0x004;      // effect 2 (snow flakes)
    if (snow > 0.3f)  mask |= 0x008;      // effect 3 (heavy snow)
    if (wind > 0.5f)  mask |= 0x020;      // effect 5 (wind base)
    if (dust > 0.1f)  mask |= 0x040;      // effect 6 (desert dust)
    if (snow > 0.1f && dust > 0.1f) mask |= 0x080; // effect 7 (snow dust)

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
                   g_oWind.active.load();
    return rainDriven && !others;
}

static void TickRainOnly(long long self, const ResolvedEnv& env, int nullSent) {
    if (!g_pActivateEffect || !g_pSetIntensity || !env.particleMgr) return;
    float rain = g_oRain.active.load() ? g_oRain.value.load() : 0.0f;

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

static void ApplyNoWindPolicy(long long self, const ResolvedEnv& env) {
    At<int>(self, WCO::SOUND_WIND)    = 0;
    At<int>(self, WCO::SOUND_SKYWIND) = 0;
    if (env.windNode) {
        At<float>(env.windNode, WN::SPEED) = 0.0f;
        At<float>(env.windNode, WN::GUST)  = 0.0f;
    }
}

// Process wind state hook.
unsigned long long __fastcall Hooked_ProcessWindState(long long self) {
    unsigned long long ret = g_pOrigProcessWindState ? g_pOrigProcessWindState(self) : 0ULL;
    if (!g_modEnabled.load()) return ret;
    const ResolvedEnv env = ResolveEnv();
    if (env.valid) {
        CaptureCloudBaseline(env);
        ApplyCloudOverrides(env);
        ApplyCelestialOverrides(env);
    }
    return ret;
}

static void ApplyWindFromSlider(long long self, const ResolvedEnv& env) {
    if (!env.valid) return;
    if (!g_oWind.active.load()) return;

    float wSpd = max(0.0f, g_oWind.value.load());
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

    const ResolvedEnv env = ResolveEnv();
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
                    else if (i == 5) effI = g_oWind.active.load() ? min(1.f, g_oWind.value.load() / 10.f) : 0.0f;
                    else if (i == 6) effI = g_oDust.active.load() ? g_oDust.value.load() : 0.0f;
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
    TickWeatherState(self, dt);
    g_pOriginalTick(self, dt);
    if (rainOnlyControl && g_activeWeather == kCustomWeather) {
        if (env.valid) {
            const int nullSent = g_pNullSentinel ? *g_pNullSentinel : 0;
            TickRainOnly(self, env, nullSent);
        }
    }
    if (resetStopNow) {
        StopAllWeatherEffects(self);
    }
    if (env.valid) {
        CaptureCloudBaseline(env);
        ApplyCloudOverrides(env);
        ApplyCelestialOverrides(env);
    }
    if (g_noWind.load()) {
        ApplyNoWindPolicy(self, env);
    }
    TickTimeControl();
}


