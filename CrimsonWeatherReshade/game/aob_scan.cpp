#include "pch.h"
#include "runtime_shared.h"

// AOB scan and hook installation
#if defined(CW_VERBOSE_AOB)
#define CW_AOB_VERBOSE_LOG(...) Log(__VA_ARGS__)
#else
#define CW_AOB_VERBOSE_LOG(...) do {} while (0)
#endif

#if defined(CW_WIND_ONLY)
static constexpr bool kEnableWeatherTickHook = false;
static constexpr bool kEnableGameplayHooks = false;
static constexpr bool kEnableIntensityHooks = true;
static constexpr bool kEnableWindHooks = false;
static constexpr bool kEnableProcessWindHook = false;
static constexpr bool kEnableWindPackHook = false;
static constexpr bool kEnableFrameHooks = false;
#else
static constexpr bool kEnableWeatherTickHook = true;
static constexpr bool kEnableGameplayHooks = true;
static constexpr bool kEnableIntensityHooks = true;
static constexpr bool kEnableWindHooks = true;
static constexpr bool kEnableProcessWindHook = true;
static constexpr bool kEnableWindPackHook = true;
static constexpr bool kEnableFrameHooks = true;
#endif

static bool ParsePattern(const char*pat,uint8_t*bytes,uint8_t*mask,size_t&len){
    len=0;const char*p=pat;
    while(*p){while(*p==' ')p++;if(!*p)break;
        if(p[0]=='?'&&p[1]=='?'){bytes[len]=0;mask[len]=0;len++;p+=2;}
        else{auto h=[](char c)->uint8_t{return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c-'A'+10;};
            bytes[len]=(h(p[0])<<4)|h(p[1]);mask[len]=0xFF;len++;p+=2;}}
    return len>0;}

static uintptr_t ScanModule(const char*pat){
    uint8_t bytes[256],mask[256];size_t len=0;
    if(!ParsePattern(pat,bytes,mask,len))return 0;
    auto*hMod=reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto*dos=reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
    auto*nt=reinterpret_cast<IMAGE_NT_HEADERS*>(hMod+dos->e_lfanew);
    auto*sec=IMAGE_FIRST_SECTION(nt);
    for(int i=0;i<nt->FileHeader.NumberOfSections;i++,sec++){
        if(!(sec->Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        uintptr_t base=reinterpret_cast<uintptr_t>(hMod)+sec->VirtualAddress;
        auto*mem=reinterpret_cast<const uint8_t*>(base);
        size_t sz=sec->Misc.VirtualSize;
        for(size_t j=0;j<=sz-len;j++){
            bool ok=true;for(size_t k=0;k<len;k++)if(mask[k]&&mem[j+k]!=bytes[k]){ok=false;break;}
            if(ok)return base+j;}
    }return 0;}

static uintptr_t ReadCall(uintptr_t a){
    if(*reinterpret_cast<uint8_t*>(a)!=0xE8)return 0;
    return a+5+*reinterpret_cast<int32_t*>(a+1);}
static uintptr_t ReadRIP7(uintptr_t a){
    return a+7+*reinterpret_cast<int32_t*>(a+3);}
static uintptr_t ReadRIP6(uintptr_t a){
    return a+6+*reinterpret_cast<int32_t*>(a+2);}
uintptr_t FindFunctionStartViaUnwind(uintptr_t pc);
static bool ReadBytesSafe(uintptr_t addr, uint8_t* out, size_t n);

static uintptr_t PromoteToFunctionStart(uintptr_t addr, const char* name) {
    if (!addr) return 0;
    uintptr_t fn = FindFunctionStartViaUnwind(addr);
    if (fn && fn != addr) {
        Log("[AOB] %s(entry) = %p <- %p\n", name, (void*)fn, (void*)addr);
        return fn;
    }
    return fn ? fn : addr;
}

static bool LooksLikeAtmosFogBlend(uintptr_t f){
    if(!f) return false;
    __try {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(f);
        if (p[0] != 0x48 || p[1] != 0x83 || p[2] != 0xEC || p[3] != 0x38) return false;
        if (p[4] != 0x48 || p[5] != 0x8B) return false;
        int idx = 0;
        if (p[6] == 0x41 && p[7] == 0x88) {
            idx = 8;
        } else if (p[6] == 0x81 && p[7] == 0x88 && p[8] == 0x00 && p[9] == 0x00 && p[10] == 0x00) {
            idx = 11;
        } else {
            return false;
        }
        if (p[idx] != 0x0F) return false;
        if (p[idx + 1] != 0x29 && p[idx + 1] != 0x11 && p[idx + 1] != 0x28 && p[idx + 1] != 0x10) return false;
        if (p[idx + 2] != 0x74 || p[idx + 3] != 0x24 || p[idx + 4] != 0x20) return false;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool LooksLikeWindPack(uintptr_t f) {
    if (!f) return false;
    uint8_t wb[40] = {};
    if (!ReadBytesSafe(f, wb, sizeof(wb))) return false;

    const uint8_t oldShape[] = {
        0x48, 0x83, 0xEC, 0x18, 0x48, 0x8B, 0x01, 0x49, 0x8B, 0xC9,
        0x48, 0x85, 0xC0, 0x49, 0x8B, 0xD2, 0xB9, 0x40, 0x00, 0x00,
        0x00, 0x4C, 0x8D, 0x40, 0x18, 0x4C, 0x0F, 0x44, 0xC1
    };
    if (memcmp(wb, oldShape, sizeof(oldShape)) == 0) return true;

    const uint8_t newShape[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30,
        0x48, 0x8B, 0x01, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xC0, 0x48,
        0x8B, 0xFA, 0xB9, 0x40, 0x00, 0x00, 0x00, 0x4C, 0x8D, 0x40,
        0x18, 0x4C, 0x0F, 0x44, 0xC1
    };
    return memcmp(wb, newShape, sizeof(newShape)) == 0;
}

static bool ReadBytesSafe(uintptr_t addr, uint8_t* out, size_t n){
    if(!addr || !out || n==0) return false;
    __try {
        memcpy(out, reinterpret_cast<const void*>(addr), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(out, 0, n);
        return false;
    }
}

static bool IsExecutableAddress(uintptr_t addr) {
    if (!addr) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD prot = mbi.Protect & 0xFFu;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
           prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
}

static bool IsReadableAddress(uintptr_t addr, size_t bytes) {
    if (!addr || bytes == 0) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD prot = mbi.Protect & 0xFFu;
    if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) return false;
    const uintptr_t regionStart = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    const uintptr_t regionEnd = regionStart + mbi.RegionSize;
    return addr >= regionStart && (addr + bytes) <= regionEnd;
}

static bool LooksLikeFunctionEntry(uintptr_t addr) {
    if (!IsExecutableAddress(addr)) return false;
    uintptr_t fn = FindFunctionStartViaUnwind(addr);
    if (fn && fn != addr) return false;
    uint8_t b[8] = {};
    if (!ReadBytesSafe(addr, b, sizeof(b))) return false;
    return
        (b[0] == 0x48 && b[1] == 0x8B) ||
        (b[0] == 0x48 && b[1] == 0x83) ||
        (b[0] == 0x48 && b[1] == 0x89) ||
        (b[0] == 0x40 && b[1] == 0x53) ||
        (b[0] == 0x55);
}

static bool TryReadIntSafe(const int* ptr, int* outValue) {
    if (!ptr || !outValue) return false;
    __try {
        *outValue = *ptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outValue = 0;
        return false;
    }
}

static bool IsValidatedCallTarget(uintptr_t addr) {
    if (!IsExecutableAddress(addr)) return false;
    if (LooksLikeFunctionEntry(addr)) return true;
    return FindFunctionStartViaUnwind(addr) != 0;
}

static RuntimeHealthState AggregateTargetHealth(std::initializer_list<AobTargetId> ids, std::string& outNote) {
    bool anyReady = false;
    bool anyDegraded = false;
    bool anyDisabled = false;
    outNote.clear();
    for (AobTargetId id : ids) {
        const RuntimeHealthEntry& entry = g_aobTargetHealth[static_cast<size_t>(id)];
        switch (entry.state) {
        case RuntimeHealthState::Ready:
            anyReady = true;
            break;
        case RuntimeHealthState::Degraded:
            anyDegraded = true;
            if (outNote.empty()) outNote = std::string(AobTargetLabel(id)) + ": " + entry.note;
            break;
        case RuntimeHealthState::Disabled:
        default:
            anyDisabled = true;
            if (outNote.empty()) outNote = std::string(AobTargetLabel(id)) + ": " + entry.note;
            break;
        }
    }

    if (!anyDisabled && !anyDegraded) return RuntimeHealthState::Ready;
    if (!anyReady && !anyDegraded) return RuntimeHealthState::Disabled;
    return RuntimeHealthState::Degraded;
}

static void RecomputeRuntimeHealthSummary() {
    std::string note;

#if defined(CW_WIND_ONLY)
    SetRuntimeGroupHealth(RuntimeHealthGroup::CoreWeather,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeGroupHealth(RuntimeHealthGroup::CloudExperiment,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeGroupHealth(RuntimeHealthGroup::Fog,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeGroupHealth(RuntimeHealthGroup::Time,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeGroupHealth(RuntimeHealthGroup::Infra,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");

    SetRuntimeFeatureHealth(RuntimeFeatureId::ForceClear,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeFeatureHealth(RuntimeFeatureId::Rain,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeFeatureHealth(RuntimeFeatureId::Dust,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeFeatureHealth(RuntimeFeatureId::Snow,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeFeatureHealth(RuntimeFeatureId::TimeControls,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeFeatureHealth(RuntimeFeatureId::CloudControls,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeFeatureHealth(RuntimeFeatureId::FogControls,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeFeatureHealth(RuntimeFeatureId::WindControls,
        AggregateTargetHealth({
            AobTargetId::GetDustIntensity
        }, note), note);
    SetRuntimeFeatureHealth(RuntimeFeatureId::NoWindControls,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeFeatureHealth(RuntimeFeatureId::DetailControls,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeFeatureHealth(RuntimeFeatureId::ExperimentControls,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeFeatureHealth(RuntimeFeatureId::CelestialControls,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    SetRuntimeFeatureHealth(RuntimeFeatureId::NativeToast,
        RuntimeHealthState::Disabled,
        "Not included in Wind Only build");
    return;
#endif

    SetRuntimeGroupHealth(RuntimeHealthGroup::CoreWeather,
        AggregateTargetHealth({
            AobTargetId::WeatherTick,
            AobTargetId::GetRainIntensity,
            AobTargetId::GetSnowIntensity,
            AobTargetId::GetDustIntensity,
            AobTargetId::ActivateEffect,
            AobTargetId::SetIntensity,
            AobTargetId::EnvManagerPtr,
            AobTargetId::NullSentinel
        }, note), note);

    SetRuntimeGroupHealth(RuntimeHealthGroup::CloudExperiment,
        AggregateTargetHealth({
            AobTargetId::ProcessWindState,
            AobTargetId::WindPack,
            AobTargetId::EnvManagerPtr
        }, note), note);

    SetRuntimeGroupHealth(RuntimeHealthGroup::Fog,
        AggregateTargetHealth({
            AobTargetId::WeatherFrameUpdate,
            AobTargetId::AtmosFogBlend
        }, note), note);

    SetRuntimeGroupHealth(RuntimeHealthGroup::Time,
        AggregateTargetHealth({
            AobTargetId::EnvManagerPtr,
            AobTargetId::TimeStores,
            AobTargetId::TimeDebugHandler
        }, note), note);

    SetRuntimeGroupHealth(RuntimeHealthGroup::Infra,
        AggregateTargetHealth({
            AobTargetId::NativeToast,
            AobTargetId::MinimapRegionLabels,
            AobTargetId::PostProcessLayerUpdate,
            AobTargetId::GetLayerMeta
        }, note), note);

    SetRuntimeFeatureHealth(RuntimeFeatureId::ForceClear,
        AggregateTargetHealth({
            AobTargetId::WeatherTick,
            AobTargetId::ActivateEffect,
            AobTargetId::SetIntensity,
            AobTargetId::EnvManagerPtr
        }, note), note);

    SetRuntimeFeatureHealth(RuntimeFeatureId::Rain,
        AggregateTargetHealth({
            AobTargetId::WeatherTick,
            AobTargetId::GetRainIntensity,
            AobTargetId::ActivateEffect,
            AobTargetId::SetIntensity
        }, note), note);

    SetRuntimeFeatureHealth(RuntimeFeatureId::Dust,
        AggregateTargetHealth({
            AobTargetId::WeatherTick,
            AobTargetId::GetDustIntensity,
            AobTargetId::ActivateEffect,
            AobTargetId::SetIntensity
        }, note), note);

    SetRuntimeFeatureHealth(RuntimeFeatureId::Snow,
        AggregateTargetHealth({
            AobTargetId::WeatherTick,
            AobTargetId::GetSnowIntensity,
            AobTargetId::ActivateEffect,
            AobTargetId::SetIntensity
        }, note), note);

    SetRuntimeFeatureHealth(RuntimeFeatureId::TimeControls,
        AggregateTargetHealth({
            AobTargetId::EnvManagerPtr,
            AobTargetId::TimeStores
        }, note), note);

    SetRuntimeFeatureHealth(RuntimeFeatureId::CloudControls,
        AggregateTargetHealth({
            AobTargetId::ProcessWindState,
            AobTargetId::WindPack,
            AobTargetId::EnvManagerPtr
        }, note), note);

    SetRuntimeFeatureHealth(RuntimeFeatureId::FogControls,
        AggregateTargetHealth({
            AobTargetId::WeatherFrameUpdate,
            AobTargetId::AtmosFogBlend
        }, note), note);

    SetRuntimeFeatureHealth(RuntimeFeatureId::WindControls,
        AggregateTargetHealth({
            AobTargetId::WeatherTick,
            AobTargetId::ProcessWindState,
            AobTargetId::WindPack,
            AobTargetId::EnvManagerPtr
        }, note), note);

    SetRuntimeFeatureHealth(RuntimeFeatureId::NoWindControls,
        AggregateTargetHealth({
            AobTargetId::WeatherTick,
            AobTargetId::GetDustIntensity,
            AobTargetId::ProcessWindState,
            AobTargetId::EnvManagerPtr
        }, note), note);

    SetRuntimeFeatureHealth(RuntimeFeatureId::DetailControls,
        AggregateTargetHealth({
            AobTargetId::WeatherTick,
            AobTargetId::EnvManagerPtr
        }, note), note);

    SetRuntimeFeatureHealth(RuntimeFeatureId::ExperimentControls,
        AggregateTargetHealth({
            AobTargetId::WindPack
        }, note), note);

    SetRuntimeFeatureHealth(RuntimeFeatureId::CelestialControls,
        RuntimeHealthState::Disabled,
        "Disabled");

    SetRuntimeFeatureHealth(RuntimeFeatureId::NativeToast,
        AggregateTargetHealth({
            AobTargetId::NativeToast
        }, note), note);
}

static void LogRuntimeHealthSummary() {
#if defined(CW_WIND_ONLY)
    const RuntimeHealthEntry& dustEntry = g_aobTargetHealth[static_cast<size_t>(AobTargetId::GetDustIntensity)];
    size_t windOnlyReadyTargets = dustEntry.state == RuntimeHealthState::Ready ? 1 : 0;
    size_t windOnlyDegradedTargets = dustEntry.state == RuntimeHealthState::Degraded ? 1 : 0;
    size_t windOnlyDisabledTargets = dustEntry.state == RuntimeHealthState::Disabled ? 1 : 0;
    if (dustEntry.state != RuntimeHealthState::Ready) {
        Log("[AOB] target %-22s %-9s addr=%p note=%s\n",
            AobTargetLabel(AobTargetId::GetDustIntensity),
            RuntimeHealthStateLabel(dustEntry.state),
            (void*)dustEntry.addr,
            dustEntry.note.empty() ? "-" : dustEntry.note.c_str());
    }
    const RuntimeHealthEntry& windEntry = g_runtimeFeatureHealth[static_cast<size_t>(RuntimeFeatureId::WindControls)];
    if (windEntry.state != RuntimeHealthState::Ready) {
        Log("[AOB] feature %-22s %-9s note=%s\n",
            RuntimeFeatureLabel(RuntimeFeatureId::WindControls),
            RuntimeHealthStateLabel(windEntry.state),
            windEntry.note.empty() ? "-" : windEntry.note.c_str());
    }
    Log("[AOB] summary targets ready=%zu degraded=%zu disabled=%zu\n",
        windOnlyReadyTargets, windOnlyDegradedTargets, windOnlyDisabledTargets);
    return;
#endif

    size_t readyTargets = 0, degradedTargets = 0, disabledTargets = 0;
    for (size_t i = 0; i < static_cast<size_t>(AobTargetId::Count); ++i) {
        const RuntimeHealthEntry& entry = g_aobTargetHealth[i];
        if (entry.state == RuntimeHealthState::Ready) {
            ++readyTargets;
            continue;
        }
        if (entry.state == RuntimeHealthState::Degraded) ++degradedTargets;
        else ++disabledTargets;
#if defined(CW_WIND_ONLY)
        if (entry.state == RuntimeHealthState::Disabled) continue;
#endif
        Log("[AOB] target %-22s %-9s addr=%p note=%s\n",
            AobTargetLabel(static_cast<AobTargetId>(i)),
            RuntimeHealthStateLabel(entry.state),
            (void*)entry.addr,
            entry.note.empty() ? "-" : entry.note.c_str());
    }
    for (size_t i = 0; i < static_cast<size_t>(RuntimeHealthGroup::Count); ++i) {
        const RuntimeHealthEntry& entry = g_runtimeGroupHealth[i];
        if (entry.state == RuntimeHealthState::Ready) continue;
#if defined(CW_WIND_ONLY)
        if (entry.state == RuntimeHealthState::Disabled) continue;
#endif
        Log("[AOB] group  %-22s %-9s note=%s\n",
            RuntimeHealthGroupLabel(static_cast<RuntimeHealthGroup>(i)),
            RuntimeHealthStateLabel(entry.state),
            entry.note.empty() ? "-" : entry.note.c_str());
    }
    for (size_t i = 0; i < static_cast<size_t>(RuntimeFeatureId::Count); ++i) {
        const RuntimeHealthEntry& entry = g_runtimeFeatureHealth[i];
        if (entry.state == RuntimeHealthState::Ready) continue;
#if defined(CW_WIND_ONLY)
        if (entry.state == RuntimeHealthState::Disabled) continue;
#endif
        Log("[AOB] feature %-22s %-9s note=%s\n",
            RuntimeFeatureLabel(static_cast<RuntimeFeatureId>(i)),
            RuntimeHealthStateLabel(entry.state),
            entry.note.empty() ? "-" : entry.note.c_str());
    }
    Log("[AOB] summary targets ready=%zu degraded=%zu disabled=%zu\n",
        readyTargets, degradedTargets, disabledTargets);
}

static size_t FindCallsitesTo(uintptr_t target, uintptr_t* out, size_t cap){
    if(!target || !out || cap==0) return 0;
    size_t found = 0;
    auto*hMod=reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto*dos=reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
    auto*nt=reinterpret_cast<IMAGE_NT_HEADERS*>(hMod+dos->e_lfanew);
    auto*sec=IMAGE_FIRST_SECTION(nt);
    for(int i=0;i<nt->FileHeader.NumberOfSections;i++,sec++){
        if(!(sec->Characteristics&IMAGE_SCN_MEM_EXECUTE))continue;
        uintptr_t base=reinterpret_cast<uintptr_t>(hMod)+sec->VirtualAddress;
        auto*mem=reinterpret_cast<const uint8_t*>(base);
        size_t sz=sec->Misc.VirtualSize;
        if(sz < 5) continue;
        for(size_t j=0;j+5<=sz;j++){
            if(mem[j] != 0xE8) continue;
            uintptr_t site = base + j;
            if(ReadCall(site) == target){
                out[found++] = site;
                if(found >= cap) return found;
            }
        }
    }
    return found;
}

static bool FindDirectCallToTargetInRange(uintptr_t start, size_t len, uintptr_t target, uintptr_t* outSite = nullptr) {
    if (!start || !target || len < 5) return false;
    __try {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(start);
        for (size_t i = 0; i + 5 <= len; ++i) {
            if (p[i] != 0xE8) continue;
            int32_t rel = *reinterpret_cast<const int32_t*>(p + i + 1);
            uintptr_t site = start + i;
            uintptr_t dst = site + 5 + rel;
            if (dst == target) {
                if (outSite) *outSite = site;
                return true;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

static bool ResolveNativeToastBridgeAOB() {
    auto* hMod = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(hMod + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);

    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;

        uintptr_t base = reinterpret_cast<uintptr_t>(hMod) + sec->VirtualAddress;
        auto* mem = reinterpret_cast<const uint8_t*>(base);
        size_t sz = sec->Misc.VirtualSize;
        if (sz < 0x60) continue;

        for (size_t j = 0; j + 0x60 <= sz; ++j) {
            const uint8_t* p = mem + j;
            if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x05) continue;
            if (p[7] != 0x48 || p[8] != 0x8B || p[9] != 0x48) continue;
            if (p[11] != 0x48 || p[12] != 0x8B || p[13] != 0x99) continue;

            uintptr_t site = base + j;
            uintptr_t rootGlobal = ReadRIP7(site);
            uint32_t outerOffset = 0;
            uint32_t managerOffset = 0;
            __try {
                outerOffset = static_cast<uint32_t>(*reinterpret_cast<const uint8_t*>(site + 10));
                managerOffset = *reinterpret_cast<const uint32_t*>(site + 14);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                continue;
            }

            uintptr_t createFn = 0;
            uintptr_t pushFn = 0;
            uintptr_t releaseFn = 0;
            const size_t remaining = sz - j;
            const size_t windowEnd = (remaining < 0x90) ? remaining : size_t(0x90);

            for (size_t k = 18; k + 8 <= windowEnd; ++k) {
                if (!createFn &&
                    p[k + 0] == 0x48 && p[k + 1] == 0x8B && p[k + 2] == 0xC8 &&
                    p[k + 3] == 0xE8) {
                    createFn = ReadCall(site + k + 3);
                    continue;
                }

                if (!pushFn &&
                    p[k + 0] == 0x48 && p[k + 1] == 0x8B && p[k + 2] == 0xCB &&
                    p[k + 3] == 0xE8) {
                    pushFn = ReadCall(site + k + 3);
                    continue;
                }

                if (!releaseFn &&
                    p[k + 0] == 0x48 && p[k + 1] == 0x8B &&
                    p[k + 2] == 0x4C && p[k + 3] == 0x24) {
                    size_t callOffset = k + 5;
                    while (callOffset < windowEnd && p[callOffset] == 0x90) ++callOffset;
                    if (callOffset + 5 <= windowEnd && p[callOffset] == 0xE8) {
                        releaseFn = ReadCall(site + callOffset);
                    }
                }
            }

            if (!rootGlobal || !createFn || !pushFn || !releaseFn || !outerOffset || !managerOffset) {
                continue;
            }

            g_pNativeToastRootGlobal = reinterpret_cast<void**>(rootGlobal);
            g_nativeToastOuterOffset = outerOffset;
            g_nativeToastManagerOffset = static_cast<ptrdiff_t>(managerOffset);
            g_pNativeToastCreateString = reinterpret_cast<NativeToastCreateString_fn>(createFn);
            g_pNativeToastPush = reinterpret_cast<NativeToastPush_fn>(pushFn);
            g_pNativeToastReleaseString = reinterpret_cast<NativeToastReleaseString_fn>(releaseFn);

            Log("[AOB] NativeToast root=%p outer=0x%X manager=0x%X create=%p push=%p release=%p\n",
                (void*)g_pNativeToastRootGlobal,
                (unsigned)outerOffset,
                (unsigned)managerOffset,
                (void*)createFn,
                (void*)pushFn,
                (void*)releaseFn);
            return true;
        }
    }

    Log("[W] AOB: NativeToast bridge not found\n");
    return false;
}

static bool LooksLikeCloudPack(uintptr_t fn, uintptr_t rain, uintptr_t dust) {
    if (!fn || !rain || !dust) return false;
    uint8_t b[16] = {};
    if (!ReadBytesSafe(fn, b, sizeof(b))) return false;
    if (!(b[0] == 0x48 && b[1] == 0x89 && b[2] == 0x5C && b[3] == 0x24)) return false;

    uintptr_t rainSite = 0;
    if (!FindDirectCallToTargetInRange(fn, 0x80, rain, &rainSite)) return false;
    if (!FindDirectCallToTargetInRange(rainSite + 5, 0x40, dust, nullptr)) return false;
    return true;
}

static uintptr_t ResolveCloudPackByCallPair(uintptr_t rain, uintptr_t dust) {
    if (!rain || !dust) return 0;
    uintptr_t xrefs[256] = {};
    size_t n = FindCallsitesTo(rain, xrefs, 256);
    for (size_t i = 0; i < n; ++i) {
        uintptr_t site = xrefs[i];
        uintptr_t fn = FindFunctionStartViaUnwind(site);
        if (!fn) continue;
        if (site < fn || (site - fn) > 0x90) continue;
        if (LooksLikeCloudPack(fn, rain, dust)) {
            return fn;
        }
    }
    return 0;
}

uintptr_t FindFunctionStartViaUnwind(uintptr_t pc){
    if (!pc) return 0;
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry((DWORD64)pc, &imageBase, nullptr);
    if (!rf || !imageBase) return 0;
    return (uintptr_t)(imageBase + rf->BeginAddress);
}

static bool LooksLikeWeatherFrameUpdate(uintptr_t addr){
    uint8_t b[12] = {};
    if (!ReadBytesSafe(addr, b, sizeof(b))) return false;
    if (b[0] != 0x48 || b[1] != 0x8B || b[2] != 0xC4) return false;
    for (int i = 3; i <= 9; ++i) {
        if (b[i] == 0x48 && b[i + 1] == 0x8D) return true;
    }
    return false;
}

static uintptr_t FindFuncStartByPrologueBack(uintptr_t from, size_t maxBack){
    if (!from) return 0;
    for (size_t back = 0; back <= maxBack; ++back) {
        uintptr_t a = from - back;
        if (LooksLikeWeatherFrameUpdate(a)) {
            return a;
        }
    }
    return 0;
}

static bool TryDeriveTimeVtableOffsetsFromFunction(uintptr_t fnStart,
                                                   ptrdiff_t& outEnvGetEntity,
                                                   ptrdiff_t& outEnvGetTime,
                                                   ptrdiff_t& outEntSetTime) {
    uint8_t buf[0x120] = {};
    if (!ReadBytesSafe(fnStart, buf, sizeof(buf))) return false;
    ptrdiff_t envGetTime = 0;
    ptrdiff_t envGetEntity = 0;
    ptrdiff_t entSetTime = 0;

    // call qword ptr [rax+disp32] -> env get-time
    for (size_t i = 0; i + 6 < sizeof(buf); ++i) {
        if (buf[i] == 0xFF && buf[i + 1] == 0x90) {
            int32_t d = *reinterpret_cast<int32_t*>(&buf[i + 2]);
            if (d > 0x80 && d < 0x300 && (d % 8) == 0) { envGetTime = d; break; }
        }
    }
    // call qword ptr [rax+disp8] -> env get-entity
    for (size_t i = 0; i + 2 < sizeof(buf); ++i) {
        if (buf[i] == 0xFF && buf[i + 1] == 0x50) {
            int8_t d = static_cast<int8_t>(buf[i + 2]);
            if (d > 0x20 && d <= 0x80 && (d % 8) == 0) { envGetEntity = d; break; }
        }
    }
    // jmp/call qword ptr [rdx+disp32] -> entity set-time
    for (size_t i = 0; i + 6 < sizeof(buf); ++i) {
        if (buf[i] == 0xFF && (buf[i + 1] == 0xA2 || buf[i + 1] == 0x92)) {
            int32_t d = *reinterpret_cast<int32_t*>(&buf[i + 2]);
            if (d > 0x80 && d < 0x300 && (d % 8) == 0) { entSetTime = d; break; }
        }
    }

    if (envGetEntity != 0x40) return false;
    if (envGetTime <= 0 || entSetTime <= 0) return false;
    if (envGetTime == entSetTime) return false;

    outEnvGetEntity = envGetEntity;
    outEnvGetTime = envGetTime;
    outEntSetTime = entSetTime;
    return true;
}

static uintptr_t FindTimeDebugHandlerAOB(ptrdiff_t& outEnvGetEntity,
                                         ptrdiff_t& outEnvGetTime,
                                         ptrdiff_t& outEntSetTime) {
    outEnvGetEntity = TD::ENV_GET_ENTITY_DEF;
    outEnvGetTime = TD::ENV_GET_TIME_DEF;
    outEntSetTime = TD::ENT_SET_TIME_DEF;

    uintptr_t site = ScanModule(
        "48 8B 49 30 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B 4B 30 0F 28 F0 F3 0F 59 35 ?? ?? ?? ?? 48 8B 01 F3 0F 59 35 ?? ?? ?? ?? FF 50 ?? 0F 28 CE 48 8B C8 48 8B 10"
    );
    if (!site) {
        site = ScanModule(
            "48 8B 49 30 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B 4B 30 0F 28 F0 48 8B 01 FF 50 ?? 0F 28 CE 48 8B C8 48 8B 10"
        );
        if (!site) return 0;
    }
    uintptr_t fn = FindFunctionStartViaUnwind(site);
    if (!fn) return 0;

    ptrdiff_t envEnt = 0, envTime = 0, entSet = 0;
    if (!TryDeriveTimeVtableOffsetsFromFunction(fn, envEnt, envTime, entSet)) return 0;
    outEnvGetEntity = envEnt;
    outEnvGetTime = envTime;
    outEntSetTime = entSet;
    return fn;
}

static bool DiscoverTimeLayoutAOB() {
    bool ok = true;

    auto extractStoreDisp = [](uintptr_t site, int32_t& outDisp) {
        for (int i = 0; i < 0x60; ++i) {
            uint8_t* p = reinterpret_cast<uint8_t*>(site + i);
            if (p[0] == 0xF3 && p[1] == 0x0F && p[2] == 0x11 && (p[3] & 0xC0) == 0x80) {
                outDisp = *reinterpret_cast<int32_t*>(p + 4);
                return true;
            }
            if (p[0] == 0xC5 && p[1] == 0xFA && p[2] == 0x11 && (p[3] & 0xC0) == 0x80) {
                outDisp = *reinterpret_cast<int32_t*>(p + 4);
                return true;
            }
        }
        return false;
    };

    uintptr_t lowSite = ScanModule(
        "40 57 48 83 EC 20 83 7A 08 02 48 8B FA 72 ?? 48 8B 09 48 89 5C 24 30 48 8B 01 FF 50 40 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? C5 FB 5A C8 C5 FA 11 8B D4 03 00 00"
    );
    uintptr_t uppSite = ScanModule(
        "40 57 48 83 EC 20 83 7A 08 02 48 8B FA 72 ?? 48 8B 09 48 89 5C 24 30 48 8B 01 FF 50 40 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? C5 FB 5A C8 C5 FA 11 8B D8 03 00 00"
    );
    if (!lowSite) lowSite = ScanModule("F3 0F 11 ?? CC 03 00 00");
    if (!uppSite) uppSite = ScanModule("F3 0F 11 ?? D0 03 00 00");
    if (!lowSite) lowSite = ScanModule("C5 FA 11 ?? D4 03 00 00");
    if (!uppSite) uppSite = ScanModule("C5 FA 11 ?? D8 03 00 00");
    if (!lowSite) lowSite = ScanModule("C5 FA 11 ?? CC 03 00 00");
    if (!uppSite) uppSite = ScanModule("C5 FA 11 ?? D0 03 00 00");
    if (!lowSite || !uppSite) {
        Log("[W] TimeAOB: lower/upper limit stores not found\n");
        ok = false;
    } else {
        int32_t lowOff = 0;
        int32_t uppOff = 0;
        if (!extractStoreDisp(lowSite, lowOff) || !extractStoreDisp(uppSite, uppOff)) {
            Log("[W] TimeAOB: failed to extract lower/upper store offsets\n");
            ok = false;
        }
        g_tdLowerLimit = static_cast<ptrdiff_t>(lowOff);
        g_tdUpperLimit = static_cast<ptrdiff_t>(uppOff);
        // Current fields are adjacent in current family.
        g_tdCurrentA = g_tdLowerLimit - 0x4;
        g_tdCurrentB = g_tdLowerLimit - 0x8;
        g_addrTimeLowerHandler = FindFunctionStartViaUnwind(lowSite);
        g_addrTimeUpperHandler = FindFunctionStartViaUnwind(uppSite);
        Log("[AOB] TimeLowerLimitStore = %p off=0x%X fn=%p\n",
            (void*)lowSite, (unsigned)lowOff, (void*)g_addrTimeLowerHandler);
        Log("[AOB] TimeUpperLimitStore = %p off=0x%X fn=%p\n",
            (void*)uppSite, (unsigned)uppOff, (void*)g_addrTimeUpperHandler);
    }

    ptrdiff_t envGetEntity = 0, envGetTime = 0, entSetTime = 0;
    g_addrTimeDebugHandler = FindTimeDebugHandlerAOB(envGetEntity, envGetTime, entSetTime);
    if (!g_addrTimeDebugHandler) {
        Log("[AOB] TimeAOB: using default vtable offsets (envGetEntity=0x%X envGetTime=0x%X entSetTime=0x%X)\n",
            (unsigned)g_tdEnvGetEntity, (unsigned)g_tdEnvGetTime, (unsigned)g_tdEntSetTime);
    } else {
        g_tdEnvGetEntity = envGetEntity;
        g_tdEnvGetTime = envGetTime;
        g_tdEntSetTime = entSetTime;
        Log("[AOB] TimeDebugHandler = %p (envGetEntity=0x%X envGetTime=0x%X entSetTime=0x%X)\n",
            (void*)g_addrTimeDebugHandler,
            (unsigned)g_tdEnvGetEntity, (unsigned)g_tdEnvGetTime, (unsigned)g_tdEntSetTime);
    }

    if (!ok) {
        g_timeLayoutReady.store(false);
        g_timeCtrlActive.store(false);
        g_timeFreeze.store(false);
        g_timeApplyRequest.store(false);
        Log("[W] TimeAOB layout incomplete; Visual Time Override disabled\n");
        return false;
    }

    g_timeLayoutReady.store(true);
    Log("[AOB] TimeLayout ready lower=0x%X upper=0x%X curA=0x%X curB=0x%X\n",
        (unsigned)g_tdLowerLimit, (unsigned)g_tdUpperLimit,
        (unsigned)g_tdCurrentA, (unsigned)g_tdCurrentB);
    return true;
}

bool InstallHook(void*t,void*d,void**tr,const char*n,bool req){
    if(!t){Log("[%s] %s: null\n",req?"E":"W",n);return!req;}
    MH_STATUS createStatus = MH_CreateHook(t,d,tr);
    if(createStatus!=MH_OK){
        if(!req && createStatus==MH_ERROR_UNSUPPORTED_FUNCTION){
            CW_AOB_VERBOSE_LOG("[AOB] %s direct hook unsupported at %p; fallback may apply\n", n, t);
            return !req;}
        Log("[%s] Hook failed: %s create=%s (%d) target=%p detour=%p\n",
            req?"E":"W",n,MH_StatusToString(createStatus),(int)createStatus,t,d);
        return!req;}
    MH_STATUS enableStatus = MH_EnableHook(t);
    if(enableStatus!=MH_OK){
        if(!req && enableStatus==MH_ERROR_UNSUPPORTED_FUNCTION){
            CW_AOB_VERBOSE_LOG("[AOB] %s enable unsupported at %p; fallback may apply\n", n, t);
            return !req;}
        Log("[%s] Hook failed: %s enable=%s (%d) target=%p\n",
            req?"E":"W",n,MH_StatusToString(enableStatus),(int)enableStatus,t);
        return!req;}
    Log("[+] Hooked %s at %p\n",n,t);return true;}

static void** FindVtableSlotForTarget(uintptr_t target) {
    if (!target) return nullptr;
    auto* hMod = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(hMod);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(hMod + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_READ) || (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t base = reinterpret_cast<uintptr_t>(hMod) + sec->VirtualAddress;
        size_t sz = sec->Misc.VirtualSize;
        if (sz < sizeof(uintptr_t) * 3) continue;
        for (size_t off = sizeof(uintptr_t); off + sizeof(uintptr_t) * 2 <= sz; off += sizeof(uintptr_t)) {
            auto* slot = reinterpret_cast<void**>(base + off);
            uintptr_t cur = reinterpret_cast<uintptr_t>(*slot);
            if (cur != target) continue;
            uintptr_t prev = *reinterpret_cast<uintptr_t*>(base + off - sizeof(uintptr_t));
            uintptr_t next = *reinterpret_cast<uintptr_t*>(base + off + sizeof(uintptr_t));
            if (IsExecutableAddress(prev) && IsExecutableAddress(next)) {
                return slot;
            }
        }
    }
    return nullptr;
}

static bool PatchPointerSlot(void** slot, void* value) {
    if (!slot) return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    *slot = value;
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    DWORD restoreProtect = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &restoreProtect);
    return true;
}

static bool InstallWeatherTickVtableHook(uintptr_t target) {
    if (g_pOriginalTick || g_pWeatherTickVtableSlot) return true;
    void** slot = FindVtableSlotForTarget(target);
    if (!slot) {
        Log("[W] WeatherTick vtable slot not found for %p\n", (void*)target);
        return false;
    }
    g_pOriginalTick = reinterpret_cast<WeatherTick_fn>(*slot);
    if (!PatchPointerSlot(slot, reinterpret_cast<void*>(&Hooked_WeatherTick))) {
        Log("[W] WeatherTick vtable patch failed at %p\n", slot);
        g_pOriginalTick = nullptr;
        return false;
    }
    g_pWeatherTickVtableSlot = slot;
    Log("[+] Vtable-hooked WeatherTick at %p\n", slot);
    return true;
}

void RestoreRuntimePatches() {
    if (g_pWeatherTickVtableSlot && g_pOriginalTick) {
        PatchPointerSlot(g_pWeatherTickVtableSlot, reinterpret_cast<void*>(g_pOriginalTick));
    }
    g_pWeatherTickVtableSlot = nullptr;
}

bool RunAOBScan(){
    ClearRuntimeHealthState();
    const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));

#if defined(CW_WIND_ONLY)
    uintptr_t windOnlyAddrGetDust = ScanModule(
        "48 8B 41 50 41 B8 40 00 00 00 48 85 C0 41 B9 48 01 00 00 48 8D 50 18 B8 B4 01 00 00 49 0F 44 D0"
    );
    if (!windOnlyAddrGetDust) {
        Log("[E] AOB: GetDustIntensity not found\n");
        return false;
    }
    Log("[AOB] GetDustIntensity(sig) = %p\n", (void*)windOnlyAddrGetDust);

    InstallHook((void*)windOnlyAddrGetDust, (void*)&Hooked_GetDustIntensity,
                (void**)&g_pOrigGetDustIntensity, "GetDustIntensity", false);

    SetAobTargetHealth(AobTargetId::GetDustIntensity,
        (windOnlyAddrGetDust && g_pOrigGetDustIntensity) ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        windOnlyAddrGetDust,
        (windOnlyAddrGetDust && g_pOrigGetDustIntensity) ? "hook installed" : "hook unavailable");

    RecomputeRuntimeHealthSummary();
    LogRuntimeHealthSummary();
    return windOnlyAddrGetDust && g_pOrigGetDustIntensity;
#endif

    // Anchor 1: WeatherTick
    uintptr_t tick=ScanModule("48 8B C4 53 48 81 EC ?? 00 00 00 C5 F2 58 81 C8 00 00 00");
    if(!tick){
        tick=ScanModule("48 8B C4 53 48 81 EC B0 00 00 00 80 3D");
    }
    if(!tick){Log("[E] AOB: WeatherTick not found\n");return false;}
    Log("[AOB] WeatherTick = %p\n",(void*)tick);

    // Anchor 2: GetRainIntensity
    uintptr_t rain=ScanModule("48 8B 51 50 4C 8B D1");
    if(!rain){Log("[E] AOB: GetRainIntensity not found\n");return false;}
    Log("[AOB] GetRainIntensity = %p\n",(void*)rain);

    bool processWindFallbackUsed = false;
    bool windPackFallbackUsed = false;
    bool weatherFrameForcedUsed = false;
    bool weatherFramePatternFallbackUsed = false;
    bool fogForcedUsed = false;
    bool fogPatternFallbackUsed = false;
    bool envManagerValidated = false;
    bool nullSentinelValidated = false;
    auto EC=[&](ptrdiff_t off,const char*n)->uintptr_t{
        uintptr_t site=tick+off;
        if(*reinterpret_cast<uint8_t*>(site)!=0xE8){
            CW_AOB_VERBOSE_LOG("[AOB] %s: Tick+0x%X is 0x%02X, using fallback resolver\n",n,(uint32_t)off,
                *reinterpret_cast<uint8_t*>(site));return 0;}
        uintptr_t t=ReadCall(site);
        Log("[AOB] %s = %p (Tick+0x%X)\n",n,(void*)t,(uint32_t)off);
        return t;};

    uintptr_t addrProcessRain  = EC(0x0AF,"ProcessRainState");
    uintptr_t addrGetSnow      = EC(0x248,"GetSnowIntensity");
    uintptr_t addrGetDust      = EC(0x301,"GetDustIntensity");
    uintptr_t addrProcessWind  = EC(0x457,"ProcessWindState");
    addrProcessWind = PromoteToFunctionStart(addrProcessWind, "ProcessWindState");
    if (!addrGetSnow) {
        addrGetSnow = ScanModule(
            "48 8B 51 50 4C 8B D1 48 85 D2 B9 40 00 00 00 48 8D 42 18 48 0F 44 C1 41 80 7A 31 00 4C 8B 08 4D 8D 81 50 01 00 00"
        );
        if (addrGetSnow) {
            Log("[AOB] GetSnowIntensity(sig) = %p\n", (void*)addrGetSnow);
        }
    }
    if (!addrGetDust) {
        addrGetDust = ScanModule(
            "48 8B 41 50 41 B8 40 00 00 00 48 85 C0 41 B9 48 01 00 00 48 8D 50 18 B8 B4 01 00 00 49 0F 44 D0"
        );
        if (addrGetDust) {
            Log("[AOB] GetDustIntensity(sig) = %p\n", (void*)addrGetDust);
        }
    }
    if (!addrProcessWind) {
        processWindFallbackUsed = true;
        // Fallback signature for FUN_143475680 family:
        // env->weatherState->GetDustIntensity; then reads [self+0xD0].
        addrProcessWind = ScanModule(
            "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 48 8B 88 D8 0E 00 00 E8 ?? ?? ?? ?? 8B 8B D0 00 00 00"
        );
        if (!addrProcessWind) {
            addrProcessWind = ScanModule(
                "48 89 5C 24 10 56 48 83 EC 40 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 48 8B 88 E0 0E 00 00 E8 ?? ?? ?? ?? 8B 8B D0 00 00 00"
            );
        }
        if (addrProcessWind) {
            addrProcessWind = PromoteToFunctionStart(addrProcessWind, "ProcessWindState");
            Log("[AOB] ProcessWindState(sig) = %p\n", (void*)addrProcessWind);
        } else {
            Log("[W] ProcessWindState not found (No Wind may be limited)\n");
        }
    }
    uintptr_t addrActivate     = EC(0x2AA,"ActivateEffect");
    uintptr_t addrSetIntensity = EC(0x2CC,"SetIntensity");
    if (!addrActivate) {
        addrActivate = ScanModule(
            "4C 8B DC 49 89 5B 08 49 89 6B 10 49 89 73 18 57 48 83 EC 40 49 8B F1 48 8B F9 49 8B 00 4C 8B 10"
        );
        if (addrActivate) {
            Log("[AOB] ActivateEffect(sig) = %p\n", (void*)addrActivate);
        }
    }
    if (!addrSetIntensity) {
        addrSetIntensity = ScanModule(
            "89 54 24 10 53 48 83 EC 30 83 79 0C 00 48 8B D9 C5 F8 29 74 24 20"
        );
        if (addrSetIntensity) {
            Log("[AOB] SetIntensity(sig) = %p\n", (void*)addrSetIntensity);
        }
    }
    uintptr_t addrWindPack = 0;
    uintptr_t addrCloudPack = ResolveCloudPackByCallPair(rain, addrGetDust);
    if (!addrCloudPack) {
        addrCloudPack = ScanModule(
            "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 4D 8B D9 4C 8B C3 48 8B FA 48 8B F1 E8 ?? ?? ?? ?? 48 8B CE F3 0F 11 03 E8"
        );
    }
    if (addrCloudPack) {
        uintptr_t cRainSite = 0, cDustSite = 0;
        FindDirectCallToTargetInRange(addrCloudPack, 0x80, rain, &cRainSite);
        if (cRainSite) FindDirectCallToTargetInRange(cRainSite + 5, 0x40, addrGetDust, &cDustSite);
        uintptr_t cRain = cRainSite ? ReadCall(cRainSite) : 0;
        uintptr_t cDust = cDustSite ? ReadCall(cDustSite) : 0;
        Log("[AOB] CloudPack = %p\n", (void*)addrCloudPack);
        if (!cRain || cRain != rain || (addrGetDust && (!cDust || cDust != addrGetDust))) {
            Log("[W] CloudPack call validation mismatch rain=%p/%p dust=%p/%p\n",
                (void*)cRain, (void*)rain, (void*)cDust, (void*)addrGetDust);
        }

        // Resolve FUN_1432b8540 (wind constant pack) from caller of CloudPack.
        uintptr_t cpX[64] = {};
        size_t nCp = FindCallsitesTo(addrCloudPack, cpX, 64);
        for (size_t i = 0; i < nCp && !addrWindPack; ++i) {
            uintptr_t cpCall = cpX[i];
            uintptr_t cpFn = FindFunctionStartViaUnwind(cpCall);
            if (!cpFn) continue;
            uintptr_t scanStart = cpCall + 5;
            uintptr_t scanEnd = min(cpFn + 0x300, scanStart + 0x100);
            for (uintptr_t a = scanStart; a + 5 <= scanEnd; ++a) {
                if (*reinterpret_cast<uint8_t*>(a) != 0xE8) continue;
                uintptr_t t = ReadCall(a);
                if (!t || t == addrCloudPack) continue;
                uint8_t wb[7] = {};
                if (!ReadBytesSafe(t, wb, sizeof(wb))) continue;
                if (LooksLikeWindPack(t)) {
                    addrWindPack = t;
                    break;
                }
            }
        }
    } else {
        Log("[W] CloudPack not found (cloud speed may be limited)\n");
    }
    if (!addrWindPack) {
        windPackFallbackUsed = true;
        addrWindPack = ScanModule(
            "48 83 EC 18 48 8B 01 49 8B C9 48 85 C0 49 8B D2 B9 40 00 00 00 4C 8D 40 18 4C 0F 44 C1"
        );
        if (!addrWindPack) {
            addrWindPack = ScanModule(
                "48 89 5C 24 08 57 48 83 EC 30 48 8B 01 48 8B D9 48 85 C0 48 8B FA B9 40 00 00 00 4C 8D 40 18 4C 0F 44 C1"
            );
        }
    }
    if (addrWindPack) {
        Log("[AOB] WindPack = %p\n", (void*)addrWindPack);
    } else {
        Log("[W] WindPack not found (cloud speed pack override limited)\n");
    }
    uintptr_t addrPPLayerUpdate = ScanModule(
        "48 8B C4 48 89 58 10 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ??"
    );
    if (!addrPPLayerUpdate) {
        addrPPLayerUpdate = ScanModule(
            "48 8B C4 48 89 58 10 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ?? ?? ?? ??"
        );
    }
    if (addrPPLayerUpdate) {
        Log("[AOB] PostProcessLayerUpdate = %p\n", (void*)addrPPLayerUpdate);
    } else {
        Log("[W] PostProcessLayerUpdate not found (diag hook disabled)\n");
    }
    uintptr_t addrGetLayerMeta = 0;
    if (addrPPLayerUpdate && *reinterpret_cast<uint8_t*>(addrPPLayerUpdate + 0xB3) == 0xE8) {
        addrGetLayerMeta = ReadCall(addrPPLayerUpdate + 0xB3);
    }
    if (!addrGetLayerMeta) {
        addrGetLayerMeta = ScanModule(
            "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 40 0F B7 39 48 8B 1D ?? ?? ?? ??"
        );
    }
    if (addrGetLayerMeta) {
        Log("[AOB] GetLayerMeta = %p\n", (void*)addrGetLayerMeta);
    } else {
        Log("[W] GetLayerMeta not found (entry channel logs limited)\n");
    }
    uintptr_t addrWeatherFrameUpdate = 0;
    uintptr_t addrAtmosFogBlend = 0;
    if (addrPPLayerUpdate) {
        uintptr_t xrefs[32] = {};
        size_t nX = FindCallsitesTo(addrPPLayerUpdate, xrefs, 32);
        Log("[AOB] PostProcessLayerUpdate xrefs=%u\n", (unsigned)nX);
        for (size_t i = 0; i < nX && i < 8; ++i) {
            CW_AOB_VERBOSE_LOG("[AOB]   xref[%u] call@%p\n", (unsigned)i, (void*)xrefs[i]);
        }

        for (size_t i = 0; i < nX && !addrAtmosFogBlend; ++i) {
            uintptr_t callSite = xrefs[i];
            if (!addrWeatherFrameUpdate) {
                uintptr_t wfUW = FindFunctionStartViaUnwind(callSite);
                if (wfUW && LooksLikeWeatherFrameUpdate(wfUW)) {
                    addrWeatherFrameUpdate = wfUW;
                    Log("[AOB] WeatherFrameUpdate(unwind) = %p (from call@%p)\n",
                        (void*)wfUW, (void*)callSite);
                } else if (callSite > 0x316) {
                    uintptr_t wf = callSite - 0x316; // known relation in current family
                    uint8_t wb[12] = {};
                    ReadBytesSafe(wf, wb, sizeof(wb));
                    if (LooksLikeWeatherFrameUpdate(wf)) {
                        addrWeatherFrameUpdate = wf;
                        Log("[AOB] WeatherFrameUpdate(xref) = %p (from ppCall-0x316)\n", (void*)wf);
                    } else {
                        Log("[W] WeatherFrameUpdate(xref) mismatch at %p bytes=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X; trying backward scan\n",
                            (void*)wf, wb[0], wb[1], wb[2], wb[3], wb[4], wb[5], wb[6], wb[7], wb[8], wb[9], wb[10], wb[11]);
                        uintptr_t wfBack = FindFuncStartByPrologueBack(callSite, 0x800);
                        if (wfBack) {
                            addrWeatherFrameUpdate = wfBack;
                            Log("[AOB] WeatherFrameUpdate(backscan) = %p (from call@%p)\n",
                                (void*)wfBack, (void*)callSite);
                        } else if (wb[0] == 0x48 && wb[1] == 0x8B && wb[2] == 0xC4) {
                            addrWeatherFrameUpdate = wf;
                            weatherFrameForcedUsed = true;
                            Log("[W] WeatherFrameUpdate forced from xref delta: %p\n", (void*)wf);
                        }
                    }
                }
            }
            uintptr_t forcedCand = 0;

            if (callSite > 0x6E && *reinterpret_cast<uint8_t*>(callSite - 0x6E) == 0xE8) {
                forcedCand = ReadCall(callSite - 0x6E);
                uint8_t fp[8] = {};
                ReadBytesSafe(forcedCand, fp, sizeof(fp));
                CW_AOB_VERBOSE_LOG("[AOB] candidate fixed d=0x6E call@%p -> %p bytes=%02X %02X %02X %02X %02X %02X %02X %02X looks=%d\n",
                    (void*)(callSite - 0x6E), (void*)forcedCand,
                    fp[0], fp[1], fp[2], fp[3], fp[4], fp[5], fp[6], fp[7],
                    LooksLikeAtmosFogBlend(forcedCand) ? 1 : 0);
                if (LooksLikeAtmosFogBlend(forcedCand)) {
                    addrAtmosFogBlend = forcedCand;
                    Log("[AOB] AtmosFogBlend(xref-fixed) = %p (ppCall-0x6E)\n", (void*)forcedCand);
                    break;
                }
            }

            uintptr_t nearestCand = 0;
            int nearestDist = 0x7FFFFFFF;
            for (int d = 0x40; d <= 0xA0; ++d) {
                uintptr_t fogSite = callSite - d;
                if (*reinterpret_cast<uint8_t*>(fogSite) != 0xE8) continue;
                uintptr_t cand = ReadCall(fogSite);
                if (d < nearestDist) {
                    nearestDist = d;
                    nearestCand = cand;
                }
                uint8_t cp[8] = {};
                ReadBytesSafe(cand, cp, sizeof(cp));
                CW_AOB_VERBOSE_LOG("[AOB] candidate d=0x%X call@%p -> %p bytes=%02X %02X %02X %02X %02X %02X %02X %02X looks=%d\n",
                    d, (void*)fogSite, (void*)cand,
                    cp[0], cp[1], cp[2], cp[3], cp[4], cp[5], cp[6], cp[7],
                    LooksLikeAtmosFogBlend(cand) ? 1 : 0);
                if (LooksLikeAtmosFogBlend(cand)) {
                    addrAtmosFogBlend = cand;
                    Log("[AOB] AtmosFogBlend(xref) = %p (ppCall-%d at %p)\n",
                        (void*)cand, d, (void*)fogSite);
                    break;
                }
            }

            if (!addrAtmosFogBlend) {
                if (forcedCand) {
                    addrAtmosFogBlend = forcedCand;
                    fogForcedUsed = true;
                    Log("[AOB] AtmosFogBlend(fallback) = %p\n", (void*)forcedCand);
                } else if (nearestCand) {
                    addrAtmosFogBlend = nearestCand;
                    fogForcedUsed = true;
                    Log("[AOB] AtmosFogBlend(fallback) = %p\n", (void*)nearestCand);
                }
            }
        }
    }

    if (!addrAtmosFogBlend) {
        fogPatternFallbackUsed = true;
        addrAtmosFogBlend = ScanModule(
            "48 83 EC 38 48 8B 41 88 0F ?? 74 24 20 F3 0F 10 48 10 48 8B 41 90 F3 0F 10 40 10"
        );
    }
    if (!addrWeatherFrameUpdate) {
        weatherFramePatternFallbackUsed = true;
        addrWeatherFrameUpdate = ScanModule(
            "48 8B C4 ?? ?? ?? ?? 48 8D ?? ?? 48 81 EC ?? ?? ?? ??"
        );
    }
    if (addrWeatherFrameUpdate) {
        Log("[AOB] WeatherFrameUpdate = %p\n", (void*)addrWeatherFrameUpdate);
    } else {
        Log("[W] WeatherFrameUpdate not found (fog-frame force disabled)\n");
    }
    g_addrWeatherFrameUpdateResolved = addrWeatherFrameUpdate;
    if (addrAtmosFogBlend) {
        Log("[AOB] AtmosFogBlend = %p\n", (void*)addrAtmosFogBlend);
    } else {
        Log("[W] AtmosFogBlend not found (fog direct override disabled)\n");
    }

    // g_EnvManagerPtr from WeatherTick+0xB4
    uintptr_t envSite = tick+0xB4;
    if(*reinterpret_cast<uint8_t*>(envSite)==0x48){
        g_pEnvManager=reinterpret_cast<uintptr_t*>(ReadRIP7(envSite));
        envManagerValidated = IsReadableAddress(reinterpret_cast<uintptr_t>(g_pEnvManager), sizeof(uintptr_t));
        Log("[AOB] g_EnvManagerPtr = %p\n",(void*)g_pEnvManager);}
    if(!envManagerValidated){
        uintptr_t envGlobalSite = ScanModule(
            "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 48 8B 88 D8 0E 00 00"
        );
        if(!envGlobalSite){
            envGlobalSite = ScanModule(
                "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 48 8B 88 E0 0E 00 00"
            );
        }
        if(envGlobalSite){
            g_pEnvManager=reinterpret_cast<uintptr_t*>(ReadRIP7(envGlobalSite));
            envManagerValidated = IsReadableAddress(reinterpret_cast<uintptr_t>(g_pEnvManager), sizeof(uintptr_t));
            Log("[AOB] g_EnvManagerPtr(sig) = %p\n",(void*)g_pEnvManager);
        }}

    // g_NullSentinel from ProcessRainState
    if(addrProcessRain)
        for(int off=0;off<256;off++){
            auto*b=reinterpret_cast<uint8_t*>(addrProcessRain+off);
            if(b[0]==0x39&&b[1]==0x05){
                int32_t d=*reinterpret_cast<int32_t*>(b+2);
                g_pNullSentinel=reinterpret_cast<int*>(addrProcessRain+off+6+d);
                int sentinelValue = 0;
                nullSentinelValidated = IsReadableAddress(reinterpret_cast<uintptr_t>(g_pNullSentinel), sizeof(int));
                if (nullSentinelValidated) {
                    nullSentinelValidated = TryReadIntSafe(g_pNullSentinel, &sentinelValue);
                }
                Log("[AOB] g_NullSentinel = %p (=0x%08X)\n",
                    (void*)g_pNullSentinel,(uint32_t)(g_pNullSentinel? sentinelValue:0));
                break;}}
    if(!nullSentinelValidated){
        uintptr_t nullSite = ScanModule(
            "8B 05 ?? ?? ?? ?? 48 8B F9 39 01"
        );
        if(nullSite){
            g_pNullSentinel=reinterpret_cast<int*>(ReadRIP6(nullSite));
            int sentinelValue = 0;
            nullSentinelValidated = IsReadableAddress(reinterpret_cast<uintptr_t>(g_pNullSentinel), sizeof(int));
            if(nullSentinelValidated){
                nullSentinelValidated = TryReadIntSafe(g_pNullSentinel, &sentinelValue);
            }
            Log("[AOB] g_NullSentinel(sig) = %p (=0x%08X)\n",
                (void*)g_pNullSentinel,(uint32_t)(g_pNullSentinel? sentinelValue:0));
        }}

    // Time control runtime layout
    const bool timeReady = DiscoverTimeLayoutAOB();
    const bool nativeToastReady = ResolveNativeToastBridgeAOB();
    uintptr_t addrMinimapRegionLabels = ScanModule(
        "66 44 89 44 24 18 66 89 54 24 10 55 53 56 57 41 56 "
        "48 8D AC 24 80 FD FF FF 48 81 EC 80 03 00 00 "
        "41 0F B7 F8 0F B7 DA 4C 8B F1 BE FF FF 00 00"
    );
    if (addrMinimapRegionLabels) {
        Log("[AOB] MinimapRegionLabels = %p\n", (void*)addrMinimapRegionLabels);
    } else {
        Log("[W] MinimapRegionLabels not found (game HUD region ID probe disabled)\n");
    }

    g_pActivateEffect = reinterpret_cast<ActivateEffect_fn>(addrActivate);
    g_pSetIntensity   = reinterpret_cast<SetIntensity_fn>  (addrSetIntensity);
    g_pGetLayerMeta   = reinterpret_cast<GetLayerMeta_fn>  (addrGetLayerMeta);

    if (kEnableWeatherTickHook) {
        InstallHook((void*)tick,(void*)&Hooked_WeatherTick,
                    (void**)&g_pOriginalTick,"WeatherTick",false);
        if (!g_pOriginalTick) {
            InstallWeatherTickVtableHook(tick);
        }
    } else {
#if defined(CW_WIND_ONLY)
        Log("[AOB] Wind Only build: WeatherTick hook not installed\n");
#else
        Log("[W] WeatherTick hook disabled for stability\n");
#endif
    }

    if (kEnableGameplayHooks || kEnableIntensityHooks) {
#if !defined(CW_WIND_ONLY)
        InstallHook((void*)rain,(void*)&Hooked_GetRainIntensity,
                    (void**)&g_pOrigGetRainIntensity,"GetRainIntensity",false);
        if(addrGetSnow)
            InstallHook((void*)addrGetSnow,(void*)&Hooked_GetSnowIntensity,
                        (void**)&g_pOrigGetSnowIntensity,"GetSnowIntensity",false);
#endif
        if(addrGetDust)
            InstallHook((void*)addrGetDust,(void*)&Hooked_GetDustIntensity,
                        (void**)&g_pOrigGetDustIntensity,"GetDustIntensity",false);
    }
    if (kEnableGameplayHooks || kEnableWindHooks) {
        if(kEnableProcessWindHook && addrProcessWind)
            InstallHook((void*)addrProcessWind,(void*)&Hooked_ProcessWindState,
                        (void**)&g_pOrigProcessWindState,"ProcessWindState",false);
        if(kEnableWindPackHook && addrWindPack)
            InstallHook((void*)addrWindPack,(void*)&Hooked_WindPack,
                        (void**)&g_pOrigWindPack,"WindPack",false);
    }
    if (kEnableGameplayHooks || kEnableFrameHooks) {
        if(addrWeatherFrameUpdate)
            InstallHook((void*)addrWeatherFrameUpdate,(void*)&Hooked_WeatherFrameUpdate,
                        (void**)&g_pOrigWeatherFrameUpdate,"WeatherFrameUpdate",false);
        if(addrAtmosFogBlend)
            InstallHook((void*)addrAtmosFogBlend,(void*)&Hooked_AtmosFogBlend,
                        (void**)&g_pOrigAtmosFogBlend,"AtmosFogBlend",false);
        if(addrMinimapRegionLabels)
            InstallHook((void*)addrMinimapRegionLabels,(void*)&Hooked_MinimapRegionLabels,
                        (void**)&g_pOrigMinimapRegionLabels,"MinimapRegionLabels",false);
    }
    if (!kEnableGameplayHooks) {
        Log("[W] Gameplay hooks isolation flags: intensity=%d wind=%d frame=%d\n",
            kEnableIntensityHooks ? 1 : 0,
            kEnableWindHooks ? 1 : 0,
            kEnableFrameHooks ? 1 : 0);
        Log("[W] Wind hook split: process=%d windpack=%d\n",
            kEnableProcessWindHook ? 1 : 0,
            kEnableWindPackHook ? 1 : 0);
    }
    const bool weatherTickReady = tick && g_pOriginalTick;
    SetAobTargetHealth(AobTargetId::WeatherTick,
        weatherTickReady ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        tick,
        !weatherTickReady ? "required hook missing"
            : (g_pWeatherTickVtableSlot ? "vtable hook installed" : "direct hook installed"));

    SetAobTargetHealth(AobTargetId::GetRainIntensity,
        (rain && g_pOrigGetRainIntensity)
            ? RuntimeHealthState::Ready
            : RuntimeHealthState::Disabled,
        rain,
        !(rain && g_pOrigGetRainIntensity) ? "required hook missing"
            : "hook installed");

    SetAobTargetHealth(AobTargetId::GetSnowIntensity,
        (addrGetSnow && g_pOrigGetSnowIntensity)
            ? RuntimeHealthState::Ready
            : RuntimeHealthState::Disabled,
        addrGetSnow,
        !(addrGetSnow && g_pOrigGetSnowIntensity) ? "Tick+0x248 unresolved or hook failed"
            : "hook installed");

    SetAobTargetHealth(AobTargetId::GetDustIntensity,
        (addrGetDust && g_pOrigGetDustIntensity)
            ? RuntimeHealthState::Ready
            : RuntimeHealthState::Disabled,
        addrGetDust,
        !(addrGetDust && g_pOrigGetDustIntensity) ? "Tick+0x301 unresolved or hook failed"
            : "hook installed");

    const bool processWindInstalled = addrProcessWind && g_pOrigProcessWindState;
    SetAobTargetHealth(AobTargetId::ProcessWindState,
        processWindInstalled
            ? RuntimeHealthState::Ready
            : RuntimeHealthState::Disabled,
        addrProcessWind,
        !processWindInstalled ? "unresolved or hook failed"
            : (processWindFallbackUsed ? "signature-resolved + hook installed" : "hook installed"));

    SetAobTargetHealth(AobTargetId::ActivateEffect,
        addrActivate
            ? RuntimeHealthState::Ready
            : RuntimeHealthState::Disabled,
        addrActivate,
        !addrActivate ? "Tick+0x2AA unresolved"
            : "resolved");

    SetAobTargetHealth(AobTargetId::SetIntensity,
        addrSetIntensity
            ? RuntimeHealthState::Ready
            : RuntimeHealthState::Disabled,
        addrSetIntensity,
        !addrSetIntensity ? "Tick+0x2CC unresolved"
            : "resolved");

    const bool windPackInstalled = addrWindPack && g_pOrigWindPack;
    SetAobTargetHealth(AobTargetId::WindPack,
        windPackInstalled
            ? RuntimeHealthState::Ready
            : RuntimeHealthState::Disabled,
        addrWindPack,
        !windPackInstalled ? "unresolved or hook failed"
            : "hook installed");

    SetAobTargetHealth(AobTargetId::PostProcessLayerUpdate,
        RuntimeHealthState::Disabled,
        addrPPLayerUpdate,
        "diagnostic hook removed");

    SetAobTargetHealth(AobTargetId::GetLayerMeta,
        addrGetLayerMeta
            ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        addrGetLayerMeta,
        addrGetLayerMeta ? "optional diagnostic resolver ready" : "optional diagnostic resolver unavailable");

    const bool weatherFrameInstalled = addrWeatherFrameUpdate && g_pOrigWeatherFrameUpdate;
    SetAobTargetHealth(AobTargetId::WeatherFrameUpdate,
        weatherFrameInstalled
            ? RuntimeHealthState::Ready
            : RuntimeHealthState::Disabled,
        addrWeatherFrameUpdate,
        !weatherFrameInstalled ? "fog frame hook unavailable"
            : "hook installed");

    const bool fogInstalled = addrAtmosFogBlend && g_pOrigAtmosFogBlend;
    SetAobTargetHealth(AobTargetId::AtmosFogBlend,
        fogInstalled ? RuntimeHealthState::Ready
            : (weatherFrameInstalled ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled),
        addrAtmosFogBlend,
        fogInstalled ? "direct hook installed"
            : (weatherFrameInstalled ? "WeatherFrameUpdate fallback active" : "direct fog hook unavailable"));

    SetAobTargetHealth(AobTargetId::EnvManagerPtr,
        envManagerValidated ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        reinterpret_cast<uintptr_t>(g_pEnvManager),
        envManagerValidated ? "RIP global pointer storage validated" : "global pointer storage unresolved");

    SetAobTargetHealth(AobTargetId::NullSentinel,
        nullSentinelValidated ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        reinterpret_cast<uintptr_t>(g_pNullSentinel),
        nullSentinelValidated ? "sentinel pointer validated" : "sentinel pointer unresolved");

    SetAobTargetHealth(AobTargetId::TimeStores,
        timeReady ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        g_addrTimeLowerHandler,
        timeReady ? "lower/upper limit stores resolved" : "time lower/upper stores unresolved");

    SetAobTargetHealth(AobTargetId::TimeDebugHandler,
        !timeReady ? RuntimeHealthState::Disabled
            : RuntimeHealthState::Ready,
        g_addrTimeDebugHandler,
        !timeReady ? "time debug handler unresolved"
            : (g_addrTimeDebugHandler ? "debug handler + vtable offsets ready" : "default vtable offsets active"));

    SetAobTargetHealth(AobTargetId::NativeToast,
        nativeToastReady ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        reinterpret_cast<uintptr_t>(g_pNativeToastRootGlobal),
        nativeToastReady ? "native toast bridge ready" : "native toast bridge unavailable");

    const bool minimapRegionInstalled = addrMinimapRegionLabels && g_pOrigMinimapRegionLabels;
    SetAobTargetHealth(AobTargetId::MinimapRegionLabels,
        minimapRegionInstalled ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        addrMinimapRegionLabels,
        minimapRegionInstalled ? "game HUD region ID hook installed" : "game HUD region ID hook unavailable");

    RecomputeRuntimeHealthSummary();
    LogRuntimeHealthSummary();
    return true;
}


