#include "pch.h"
#include "runtime_shared.h"

namespace {

bool IsReadablePointer(uintptr_t addr, size_t bytes) {
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

} // namespace

ResolvedEnv ResolveEnv() {
    ResolvedEnv r{};
    if (!g_pEnvManager || !*g_pEnvManager) {
        return r;
    }

    __try {
        auto* envMgr = reinterpret_cast<void**>(*g_pEnvManager);
        if (!IsReadablePointer(reinterpret_cast<uintptr_t>(envMgr), sizeof(void*))) {
            return r;
        }

        auto* vtbl = *reinterpret_cast<uintptr_t**>(envMgr);
        if (!IsReadablePointer(reinterpret_cast<uintptr_t>(vtbl), 0x68)) {
            return r;
        }

        using GetEntityFn = long long(__fastcall*)(void*);
        auto getEntity = reinterpret_cast<GetEntityFn>(vtbl[0x60 / 8]);
        if (!getEntity || !IsReadablePointer(reinterpret_cast<uintptr_t>(getEntity), 16)) {
            return r;
        }

        r.entity = getEntity(envMgr);
        if (!r.entity || !IsReadablePointer(static_cast<uintptr_t>(r.entity), 0xEF0)) {
            return r;
        }

        r.weatherState = *reinterpret_cast<long long*>(r.entity + g_envWeatherStateOffset);
        if (!r.weatherState || !IsReadablePointer(static_cast<uintptr_t>(r.weatherState),
                                                   g_weatherNodeContainerOffset + sizeof(long long))) {
            return r;
        }

        long long cont = *reinterpret_cast<long long*>(r.weatherState + g_weatherNodeContainerOffset);
        if (!cont || !IsReadablePointer(static_cast<uintptr_t>(cont), 0x28)) {
            return r;
        }

        r.cloudNode = *reinterpret_cast<long long*>(cont + 0x18);
        r.windNode = *reinterpret_cast<long long*>(cont + 0x20);
        if (r.cloudNode && !IsReadablePointer(static_cast<uintptr_t>(r.cloudNode), 0x100)) {
            r.cloudNode = 0;
        }
        if (r.windNode && !IsReadablePointer(static_cast<uintptr_t>(r.windNode), WN::CHECK_SNOW_RATE + sizeof(uint8_t))) {
            r.windNode = 0;
        }

        r.particleMgr = *reinterpret_cast<long long*>(r.entity + 0xEE8);
        if (r.particleMgr && !IsReadablePointer(static_cast<uintptr_t>(r.particleMgr), 0x20)) {
            r.particleMgr = 0;
        }
        if (!r.particleMgr) {
            r.particleMgr = *reinterpret_cast<long long*>(r.entity + 0xEE0);
        }
        if (r.particleMgr && !IsReadablePointer(static_cast<uintptr_t>(r.particleMgr), 0x20)) {
            r.particleMgr = 0;
        }

        r.valid = r.entity != 0 && r.weatherState != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r = ResolvedEnv{};
    }
    return r;
}

float Clamp01(float v) {
    return min(1.0f, max(0.0f, v));
}

float NormalizeHour24(float h) {
    if (!std::isfinite(h)) {
        return 0.0f;
    }
    h = fmodf(h, 24.0f);
    if (h < 0.0f) {
        h += 24.0f;
    }
    return h;
}

bool ResolveTimeContext(void*& outEnvMgr, long long& outEntity) {
    outEnvMgr = nullptr;
    outEntity = 0;
    if (!g_pEnvManager || !*g_pEnvManager) {
        return false;
    }

    void* envMgr = reinterpret_cast<void*>(*g_pEnvManager);
    auto* vt = *reinterpret_cast<uintptr_t**>(envMgr);
    auto getEntity = reinterpret_cast<long long(__fastcall*)(void*)>(vt[g_tdEnvGetEntity / 8]);
    if (!getEntity) {
        return false;
    }

    long long entity = 0;
    __try {
        entity = getEntity(envMgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (!entity) {
        return false;
    }

    outEnvMgr = envMgr;
    outEntity = entity;
    return true;
}

bool TryReadCurrentTimeRaw(void* envMgr, float& outRaw) {
    if (!envMgr) {
        return false;
    }

    auto* vt = *reinterpret_cast<uintptr_t**>(envMgr);
    auto getTime = reinterpret_cast<EnvGetTimeOfDay_fn>(vt[g_tdEnvGetTime / 8]);
    if (!getTime) {
        return false;
    }

    __try {
        outRaw = static_cast<float>(getTime(envMgr));
        return std::isfinite(outRaw);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TrySetTimeRaw(long long entity, float raw) {
    if (!entity || !std::isfinite(raw)) {
        return false;
    }

    auto* vt = *reinterpret_cast<uintptr_t**>(entity);
    auto setTime = reinterpret_cast<EntitySetTimeOfDay_fn>(vt[g_tdEntSetTime / 8]);
    if (!setTime) {
        return false;
    }

    __try {
        setTime(entity, raw);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void CaptureTimeLimitBaseline(long long entity) {
    if (!entity || g_timeLimitsCaptured.load()) {
        return;
    }

    float lo = 0.0f;
    float hi = 1.0f;
    __try {
        lo = At<float>(entity, g_tdLowerLimit);
        hi = At<float>(entity, g_tdUpperLimit);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lo = 0.0f;
        hi = 1.0f;
    }

    if (!std::isfinite(lo) || !std::isfinite(hi)) {
        lo = 0.0f;
        hi = 1.0f;
    }

    g_timeBaseLower.store(lo);
    g_timeBaseUpper.store(hi);
    g_timeDomainHours.store(hi > 1.5f && hi <= 48.0f);
    g_timeDomainKnown.store(true);
    g_timeLimitsCaptured.store(true);
    Log("[visual-time] baseline lower=%.4f upper=%.4f domain=%s\n",
        lo,
        hi,
        g_timeDomainHours.load() ? "hours" : "normalized");
}

void RestoreTimeLimitBaseline(long long entity) {
    if (!entity || !g_timeLimitsCaptured.load()) {
        return;
    }

    __try {
        At<float>(entity, g_tdLowerLimit) = g_timeBaseLower.load();
        At<float>(entity, g_tdUpperLimit) = g_timeBaseUpper.load();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[W] visual-time restore baseline exception\n");
    }
}

float UIHourToEngineRaw(float hour) {
    hour = NormalizeHour24(hour);
    return g_timeDomainKnown.load() && !g_timeDomainHours.load() ? (hour / 24.0f) : hour;
}

bool TryReadCurrentHourFromEntity(long long entity, float& outHour) {
    if (!entity) {
        return false;
    }

    float lo = g_timeBaseLower.load();
    float hi = g_timeBaseUpper.load();
    float a = 0.0f;
    float b = 0.0f;
    __try {
        a = At<float>(entity, g_tdCurrentA);
        b = At<float>(entity, g_tdCurrentB);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    auto inRange = [&](float raw) {
        return std::isfinite(raw) && raw >= (lo - 0.5f) && raw <= (hi + 0.5f);
    };

    float raw = NAN;
    if (inRange(a) && !inRange(b)) {
        raw = a;
    } else if (inRange(b) && !inRange(a)) {
        raw = b;
    } else if (inRange(a)) {
        raw = a;
    }

    if (!std::isfinite(raw)) {
        return false;
    }

    outHour = NormalizeHour24(g_timeDomainHours.load() ? raw : raw * 24.0f);
    return true;
}

void TickTimeControl() {
    static unsigned int s_progressSetterWrites = 0;
    static unsigned int s_progressLimitWrites = 0;
    static unsigned long long s_progressDiagLastTick = 0;

    if (!g_timeLayoutReady.load()) {
        return;
    }

    void* envMgr = nullptr;
    long long entity = 0;
    if (!ResolveTimeContext(envMgr, entity)) {
        return;
    }

    CaptureTimeLimitBaseline(entity);

    float currentHour = NAN;
    if (TryReadCurrentHourFromEntity(entity, currentHour)) {
        g_timeCurrentHour.store(currentHour);
        g_timeCurrentHourValid.store(true);
    } else {
        float currentRaw = 0.0f;
        if (TryReadCurrentTimeRaw(envMgr, currentRaw)) {
            currentHour = NormalizeHour24(g_timeDomainHours.load() ? currentRaw : (currentRaw * 24.0f));
            g_timeCurrentHour.store(currentHour);
            g_timeCurrentHourValid.store(true);
        }
    }
    const bool active = g_timeCtrlActive.load();
    const bool freeze = g_timeFreeze.load();
    if (!active && !freeze) {
        g_timeOriginalHour.store(g_timeCurrentHour.load());
        g_timeOriginalHourValid.store(true);
    }
    if (!active && !freeze) {
        g_timeProgressVisualTime.store(false);
        g_timeProgressMatchGameTime.store(false);
        g_timeProgressLastTick.store(0);
        g_timeProgressMatchLastMinute.store(-1);
        g_timeProgressMatchPendingMs.store(0);
        if (g_timeFreezeApplied.exchange(false)) {
            RestoreTimeLimitBaseline(entity);
        }
        g_timeSetHoldTicks.store(0);
        return;
    }

    const bool progressVisualTime = g_timeProgressVisualTime.load();
    float targetHour = NormalizeHour24(g_timeTargetHour.load());
    if (freeze && progressVisualTime) {
        constexpr unsigned long long kProgressMinuteMs = 5000;
        if (g_timeProgressMatchGameTime.load()) {
            constexpr int kDayMinutes = 24 * 60;
            constexpr int kMaxMatchedForwardMinutes = 12 * 60;
            const unsigned long long now = GetTickCount64();
            int hudMinute = -1;
            if (g_timeUiClockSourceValid.load() && g_timeUiClockValid.load()) {
                const int hudHour = g_timeUiClockHour24.load();
                const int hudClockMinute = g_timeUiClockMinute.load();
                if (hudHour >= 0 && hudHour < 24 && hudClockMinute >= 0 && hudClockMinute < 60) {
                    hudMinute = hudHour * 60 + hudClockMinute;
                }
            }

            if (hudMinute < 0) {
                g_timeProgressMatchLastMinute.store(-1);
                g_timeProgressMatchPendingMs.store(0);
            } else {
                const int lastHudMinute = g_timeProgressMatchLastMinute.load();
                if (lastHudMinute < 0 || lastHudMinute >= kDayMinutes) {
                    targetHour = NormalizeHour24(static_cast<float>(hudMinute) / 60.0f);
                    g_timeTargetHour.store(targetHour);
                    g_timeProgressMatchLastMinute.store(hudMinute);
                    g_timeProgressMatchPendingMs.store(0);
                    g_timeProgressLastTick.store(now);
                    g_timeApplyRequest.store(true);
                } else if (hudMinute != lastHudMinute) {
                    int deltaMinutes = hudMinute - lastHudMinute;
                    if (deltaMinutes < 0) {
                        deltaMinutes += kDayMinutes;
                    }
                    g_timeProgressMatchLastMinute.store(hudMinute);

                    if (deltaMinutes == 1) {
                        const int previousPendingMs = g_timeProgressMatchPendingMs.fetch_add(static_cast<int>(kProgressMinuteMs));
                        const int maxPendingMs = static_cast<int>(kProgressMinuteMs * kMaxMatchedForwardMinutes);
                        const int totalPendingMs = previousPendingMs + static_cast<int>(kProgressMinuteMs);
                        if (previousPendingMs <= 0) {
                            g_timeProgressMatchPendingMs.store(static_cast<int>(kProgressMinuteMs));
                            g_timeProgressLastTick.store(now);
                        } else if (totalPendingMs > maxPendingMs) {
                            g_timeProgressMatchPendingMs.store(maxPendingMs);
                        }
                    } else if (deltaMinutes > 1 && deltaMinutes <= kMaxMatchedForwardMinutes) {
                        const int pendingMs = g_timeProgressMatchPendingMs.exchange(0);
                        const float pendingMinutes = max(0, pendingMs) / static_cast<float>(kProgressMinuteMs);
                        targetHour = NormalizeHour24(targetHour + (static_cast<float>(deltaMinutes) + pendingMinutes) / 60.0f);
                        g_timeTargetHour.store(targetHour);
                        g_timeProgressLastTick.store(now);
                        g_timeApplyRequest.store(true);
                    } else {
                        g_timeProgressMatchPendingMs.store(0);
                    }
                }
            }

            int pendingMs = g_timeProgressMatchPendingMs.load();
            if (pendingMs > 0) {
                const float cadenceSetting = g_timeProgressCadenceMs.load();
                const unsigned long long cadenceMs = std::isfinite(cadenceSetting) && cadenceSetting > 0.0f
                    ? static_cast<unsigned long long>(cadenceSetting)
                    : 0ull;
                unsigned long long lastTick = g_timeProgressLastTick.load();
                if (!lastTick || now < lastTick) {
                    g_timeProgressLastTick.store(now);
                } else {
                    unsigned long long elapsedMs = now - lastTick;
                    const unsigned long long maxProgressGapMs = max(10000ull, cadenceMs * 4ull);
                    if (elapsedMs > maxProgressGapMs) {
                        elapsedMs = 0;
                        g_timeProgressLastTick.store(now);
                    }
                    if (cadenceMs > 0 && elapsedMs < cadenceMs) {
                        elapsedMs = 0;
                    }
                    if (elapsedMs > 0) {
                        const int consumeMs = min(static_cast<int>(elapsedMs), pendingMs);
                        const int remainingMs = max(0, pendingMs - consumeMs);
                        g_timeProgressMatchPendingMs.store(remainingMs);
                        targetHour = NormalizeHour24(targetHour + static_cast<float>(consumeMs) / static_cast<float>(kProgressMinuteMs * 60ull));
                        g_timeTargetHour.store(targetHour);
                        g_timeProgressLastTick.store(now);
                    }
                }
            }
        } else {
            g_timeProgressMatchLastMinute.store(-1);
            g_timeProgressMatchPendingMs.store(0);
            const unsigned long long now = GetTickCount64();
            unsigned long long lastTick = g_timeProgressLastTick.load();
            if (!lastTick || now < lastTick) {
                g_timeProgressLastTick.store(now);
            } else {
                unsigned long long elapsedMs = now - lastTick;
                const float cadenceSetting = g_timeProgressCadenceMs.load();
                const unsigned long long cadenceMs = std::isfinite(cadenceSetting) && cadenceSetting > 0.0f
                    ? static_cast<unsigned long long>(cadenceSetting)
                    : 0ull;
                const unsigned long long maxProgressGapMs = max(10000ull, cadenceMs * 4ull);
                if (elapsedMs > maxProgressGapMs) {
                    elapsedMs = 0;
                    g_timeProgressLastTick.store(now);
                }
                if (cadenceMs > 0 && elapsedMs < cadenceMs) {
                    elapsedMs = 0;
                }
                if (elapsedMs > 0) {
                    const float addHours = static_cast<float>(elapsedMs) / static_cast<float>(kProgressMinuteMs * 60ull);
                    targetHour = NormalizeHour24(targetHour + addHours);
                    g_timeTargetHour.store(targetHour);
                    g_timeProgressLastTick.store(now);
                }
            }
        }
    } else {
        g_timeProgressLastTick.store(0);
        g_timeProgressMatchLastMinute.store(-1);
        g_timeProgressMatchPendingMs.store(0);
    }
    const float targetRaw = UIHourToEngineRaw(targetHour);
    const bool applyNow = g_timeApplyRequest.exchange(false);

    if (freeze) {
        const float frozenRaw = g_timeFrozenRaw.load();
        const bool freezeApplied = g_timeFreezeApplied.load();
        const bool targetChanged = fabsf(frozenRaw - targetRaw) > 0.000001f;
        const bool needWrite = !freezeApplied || applyNow || targetChanged;
        if (needWrite) {
            // The native setter is for discrete jumps. Continuous progression only moves
            // the clamp window so the game's own time update remains frame-coherent.
            if (!freezeApplied || applyNow || !progressVisualTime) {
                TrySetTimeRaw(entity, targetRaw);
                if (progressVisualTime) {
                    ++s_progressSetterWrites;
                }
            }
            __try {
                At<float>(entity, g_tdLowerLimit) = targetRaw;
                At<float>(entity, g_tdUpperLimit) = targetRaw;
                g_timeFreezeApplied.store(true);
                g_timeFrozenRaw.store(targetRaw);
                if (progressVisualTime) {
                    ++s_progressLimitWrites;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("[W] visual-time freeze write exception\n");
            }
        }
        if (progressVisualTime) {
            const unsigned long long now = GetTickCount64();
            if (!s_progressDiagLastTick) {
                s_progressDiagLastTick = now;
            } else if (now - s_progressDiagLastTick >= 10000) {
                float lower = NAN;
                float upper = NAN;
                __try {
                    lower = At<float>(entity, g_tdLowerLimit);
                    upper = At<float>(entity, g_tdUpperLimit);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    lower = NAN;
                    upper = NAN;
                }
                Log("[visual-time] progress health setterWrites=%u limitWrites=%u target=%.4f current=%.4f lower=%.4f upper=%.4f\n",
                    s_progressSetterWrites,
                    s_progressLimitWrites,
                    targetHour,
                    g_timeCurrentHour.load(),
                    lower,
                    upper);
                s_progressSetterWrites = 0;
                s_progressLimitWrites = 0;
                s_progressDiagLastTick = now;
            }
        } else {
            s_progressSetterWrites = 0;
            s_progressLimitWrites = 0;
            s_progressDiagLastTick = 0;
        }
        g_timeSetHoldTicks.store(0);
        return;
    }

    s_progressSetterWrites = 0;
    s_progressLimitWrites = 0;
    s_progressDiagLastTick = 0;
    if (g_timeFreezeApplied.exchange(false)) {
        RestoreTimeLimitBaseline(entity);
    }
    if (applyNow) {
        g_timeSetHoldTicks.store(8);
    }

    const int holdTicks = g_timeSetHoldTicks.load();
    if (holdTicks > 0) {
        TrySetTimeRaw(entity, targetRaw);
        g_timeSetHoldTicks.store(holdTicks - 1);
    }
}

void SuspendTimeControl() {
    if (!g_timeLayoutReady.load()) {
        return;
    }

    void* envMgr = nullptr;
    long long entity = 0;
    if (!ResolveTimeContext(envMgr, entity)) {
        return;
    }

    CaptureTimeLimitBaseline(entity);
    const bool hadFrozenTime = g_timeFreezeApplied.exchange(false);
    RestoreTimeLimitBaseline(entity);
    if (hadFrozenTime) {
        Log("[visual-time] suspended, baseline restored\n");
    }
    g_timeApplyRequest.store(false);
    g_timeProgressLastTick.store(0);
    g_timeProgressMatchLastMinute.store(-1);
    g_timeProgressMatchPendingMs.store(0);
    g_timeSetHoldTicks.store(0);
    g_timeFrozenRaw.store(-9999.0f);
}
