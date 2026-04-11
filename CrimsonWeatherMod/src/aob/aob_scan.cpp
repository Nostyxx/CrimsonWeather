#include "pch.h"
#include "cw_shared.h"

// AOB scan and hook installation
#if defined(CW_VERBOSE_AOB)
#define CW_AOB_VERBOSE_LOG(...) Log(__VA_ARGS__)
#else
#define CW_AOB_VERBOSE_LOG(...) do {} while (0)
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
uintptr_t FindFunctionStartViaUnwind(uintptr_t pc);

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
            AobTargetId::CloudPack,
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
            AobTargetId::TimeStores,
            AobTargetId::TimeDebugHandler
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
            AobTargetId::GetDustIntensity
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

    SetRuntimeFeatureHealth(RuntimeFeatureId::NativeToast,
        AggregateTargetHealth({
            AobTargetId::NativeToast
        }, note), note);
}

static void LogRuntimeHealthSummary() {
    for (size_t i = 0; i < static_cast<size_t>(AobTargetId::Count); ++i) {
        const RuntimeHealthEntry& entry = g_aobTargetHealth[i];
        Log("[AOB] target %-22s %-9s addr=%p note=%s\n",
            AobTargetLabel(static_cast<AobTargetId>(i)),
            RuntimeHealthStateLabel(entry.state),
            (void*)entry.addr,
            entry.note.empty() ? "-" : entry.note.c_str());
    }
    for (size_t i = 0; i < static_cast<size_t>(RuntimeHealthGroup::Count); ++i) {
        const RuntimeHealthEntry& entry = g_runtimeGroupHealth[i];
        Log("[AOB] group  %-22s %-9s note=%s\n",
            RuntimeHealthGroupLabel(static_cast<RuntimeHealthGroup>(i)),
            RuntimeHealthStateLabel(entry.state),
            entry.note.empty() ? "-" : entry.note.c_str());
    }
    for (size_t i = 0; i < static_cast<size_t>(RuntimeFeatureId::Count); ++i) {
        const RuntimeHealthEntry& entry = g_runtimeFeatureHealth[i];
        Log("[AOB] feature %-22s %-9s note=%s\n",
            RuntimeFeatureLabel(static_cast<RuntimeFeatureId>(i)),
            RuntimeHealthStateLabel(entry.state),
            entry.note.empty() ? "-" : entry.note.c_str());
    }
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

    uintptr_t lowSite = ScanModule("F3 0F 11 ?? CC 03 00 00");
    uintptr_t uppSite = ScanModule("F3 0F 11 ?? D0 03 00 00");
    if (!lowSite || !uppSite) {
        Log("[W] TimeAOB: lower/upper limit stores not found\n");
        ok = false;
    } else {
        int32_t lowOff = *reinterpret_cast<int32_t*>(lowSite + 4);
        int32_t uppOff = *reinterpret_cast<int32_t*>(uppSite + 4);
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
        Log("[W] TimeAOB: debug time handler not found\n");
        ok = false;
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
    if(MH_CreateHook(t,d,tr)!=MH_OK||MH_EnableHook(t)!=MH_OK){
        Log("[%s] Hook failed: %s\n",req?"E":"W",n);return!req;}
    Log("[+] Hooked %s at %p\n",n,t);return true;}

bool RunAOBScan(){
    ClearRuntimeHealthState();

    // Anchor 1: WeatherTick
    uintptr_t tick=ScanModule("48 8B C4 53 48 81 EC B0 00 00 00 80 3D");
    if(!tick){Log("[E] AOB: WeatherTick not found\n");return false;}
    Log("[AOB] WeatherTick = %p\n",(void*)tick);

    // Anchor 2: GetRainIntensity
    uintptr_t rain=ScanModule("48 8B 51 50 4C 8B D1");
    if(!rain){Log("[E] AOB: GetRainIntensity not found\n");return false;}
    Log("[AOB] GetRainIntensity = %p\n",(void*)rain);

    bool processWindFallbackUsed = false;
    bool cloudPackFallbackUsed = false;
    bool cloudPackValidationMismatch = false;
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
            Log("[W] %s: byte@+0x%X=0x%02X (expected E8)\n",n,(uint32_t)off,
                *reinterpret_cast<uint8_t*>(site));return 0;}
        uintptr_t t=ReadCall(site);
        Log("[AOB] %s = %p (Tick+0x%X)\n",n,(void*)t,(uint32_t)off);
        return t;};

    uintptr_t addrProcessRain  = EC(0x0AF,"ProcessRainState");
    uintptr_t addrGetSnow      = EC(0x248,"GetSnowIntensity");
    uintptr_t addrGetDust      = EC(0x301,"GetDustIntensity");
    uintptr_t addrProcessWind  = EC(0x457,"ProcessWindState");
    if (!addrProcessWind) {
        processWindFallbackUsed = true;
        // Fallback signature for FUN_143475680 family:
        // env->weatherState->GetDustIntensity; then reads [self+0xD0].
        addrProcessWind = ScanModule(
            "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 48 8B 88 D8 0E 00 00 E8 ?? ?? ?? ?? 8B 8B D0 00 00 00"
        );
        if (addrProcessWind) {
            Log("[AOB] ProcessWindState(sig) = %p\n", (void*)addrProcessWind);
        } else {
            Log("[W] ProcessWindState not found (No Wind may be limited)\n");
        }
    }
    uintptr_t addrActivate     = EC(0x2AA,"ActivateEffect");
    uintptr_t addrSetIntensity = EC(0x2CC,"SetIntensity");
    uintptr_t addrWindPack = 0;
    uintptr_t addrCloudPack = ResolveCloudPackByCallPair(rain, addrGetDust);
    if (!addrCloudPack) {
        cloudPackFallbackUsed = true;
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
            cloudPackValidationMismatch = true;
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
                if (wb[0] == 0x48 && wb[1] == 0x83 && wb[2] == 0xEC && wb[3] == 0x18 &&
                    wb[4] == 0x48 && wb[5] == 0x8B && wb[6] == 0x01) {
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
                CW_AOB_VERBOSE_LOG("[AOB]   probe fixed d=0x6E call@%p -> %p bytes=%02X %02X %02X %02X %02X %02X %02X %02X looks=%d\n",
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
                CW_AOB_VERBOSE_LOG("[AOB]   probe d=0x%X call@%p -> %p bytes=%02X %02X %02X %02X %02X %02X %02X %02X looks=%d\n",
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
                    Log("[W] AtmosFogBlend forced from fixed probe: %p\n", (void*)forcedCand);
                } else if (nearestCand) {
                    addrAtmosFogBlend = nearestCand;
                    fogForcedUsed = true;
                    Log("[W] AtmosFogBlend forced from nearest prior call: %p (d=0x%X)\n",
                        (void*)nearestCand, nearestDist);
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

    // Time control runtime layout
    const bool timeReady = DiscoverTimeLayoutAOB();
    const bool nativeToastReady = ResolveNativeToastBridgeAOB();

    g_pActivateEffect = reinterpret_cast<ActivateEffect_fn>(addrActivate);
    g_pSetIntensity   = reinterpret_cast<SetIntensity_fn>  (addrSetIntensity);
    g_pGetLayerMeta   = reinterpret_cast<GetLayerMeta_fn>  (addrGetLayerMeta);

    if(!InstallHook((void*)tick,(void*)&Hooked_WeatherTick,
                    (void**)&g_pOriginalTick,"WeatherTick",true))return false;

    InstallHook((void*)rain,(void*)&Hooked_GetRainIntensity,
                (void**)&g_pOrigGetRainIntensity,"GetRainIntensity",false);
    if(addrGetSnow)
        InstallHook((void*)addrGetSnow,(void*)&Hooked_GetSnowIntensity,
                    (void**)&g_pOrigGetSnowIntensity,"GetSnowIntensity",false);
    if(addrGetDust)
        InstallHook((void*)addrGetDust,(void*)&Hooked_GetDustIntensity,
                    (void**)&g_pOrigGetDustIntensity,"GetDustIntensity",false);
    if(addrProcessWind)
        InstallHook((void*)addrProcessWind,(void*)&Hooked_ProcessWindState,
                    (void**)&g_pOrigProcessWindState,"ProcessWindState",false);
    if(addrWindPack)
        InstallHook((void*)addrWindPack,(void*)&Hooked_WindPack,
                    (void**)&g_pOrigWindPack,"WindPack",false);
    if(addrCloudPack)
        InstallHook((void*)addrCloudPack,(void*)&Hooked_CloudPack,
                    (void**)&g_pOrigCloudPack,"CloudPack",false);
    if(addrPPLayerUpdate)
        InstallHook((void*)addrPPLayerUpdate,(void*)&Hooked_PPLayerUpdate,
                    (void**)&g_pOrigPPLayerUpdate,"PostProcessLayerUpdate",false);
    if(addrWeatherFrameUpdate)
        InstallHook((void*)addrWeatherFrameUpdate,(void*)&Hooked_WeatherFrameUpdate,
                    (void**)&g_pOrigWeatherFrameUpdate,"WeatherFrameUpdate",false);
    if(addrAtmosFogBlend)
        InstallHook((void*)addrAtmosFogBlend,(void*)&Hooked_AtmosFogBlend,
                    (void**)&g_pOrigAtmosFogBlend,"AtmosFogBlend",false);

    SetAobTargetHealth(AobTargetId::WeatherTick,
        (tick && g_pOriginalTick) ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        tick,
        (tick && g_pOriginalTick) ? "anchor + hook installed" : "required hook missing");

    SetAobTargetHealth(AobTargetId::GetRainIntensity,
        (rain && g_pOrigGetRainIntensity)
            ? (IsValidatedCallTarget(rain) ? RuntimeHealthState::Ready : RuntimeHealthState::Degraded)
            : RuntimeHealthState::Disabled,
        rain,
        !(rain && g_pOrigGetRainIntensity) ? "required hook missing"
            : (IsValidatedCallTarget(rain) ? "anchor + hook installed" : "hook installed, unwind metadata not confirmed"));

    SetAobTargetHealth(AobTargetId::GetSnowIntensity,
        (addrGetSnow && g_pOrigGetSnowIntensity)
            ? (IsValidatedCallTarget(addrGetSnow) ? RuntimeHealthState::Ready : RuntimeHealthState::Degraded)
            : RuntimeHealthState::Disabled,
        addrGetSnow,
        !(addrGetSnow && g_pOrigGetSnowIntensity) ? "Tick+0x248 unresolved or hook failed"
            : (IsValidatedCallTarget(addrGetSnow) ? "Tick+0x248 + hook installed" : "hook installed, unwind metadata not confirmed"));

    SetAobTargetHealth(AobTargetId::GetDustIntensity,
        (addrGetDust && g_pOrigGetDustIntensity)
            ? (IsValidatedCallTarget(addrGetDust) ? RuntimeHealthState::Ready : RuntimeHealthState::Degraded)
            : RuntimeHealthState::Disabled,
        addrGetDust,
        !(addrGetDust && g_pOrigGetDustIntensity) ? "Tick+0x301 unresolved or hook failed"
            : (IsValidatedCallTarget(addrGetDust) ? "Tick+0x301 + hook installed" : "hook installed, unwind metadata not confirmed"));

    const bool processWindInstalled = addrProcessWind && g_pOrigProcessWindState;
    SetAobTargetHealth(AobTargetId::ProcessWindState,
        processWindInstalled
            ? (processWindFallbackUsed ? RuntimeHealthState::Degraded :
               (IsValidatedCallTarget(addrProcessWind) ? RuntimeHealthState::Ready : RuntimeHealthState::Degraded))
            : RuntimeHealthState::Disabled,
        addrProcessWind,
        !processWindInstalled ? "unresolved or hook failed"
            : (processWindFallbackUsed ? "fallback signature + hook installed"
               : (IsValidatedCallTarget(addrProcessWind) ? "Tick+0x457 + hook installed" : "hook installed, unwind metadata not confirmed")));

    SetAobTargetHealth(AobTargetId::ActivateEffect,
        addrActivate
            ? (IsValidatedCallTarget(addrActivate) ? RuntimeHealthState::Ready : RuntimeHealthState::Degraded)
            : RuntimeHealthState::Disabled,
        addrActivate,
        !addrActivate ? "Tick+0x2AA unresolved"
            : (IsValidatedCallTarget(addrActivate) ? "Tick+0x2AA resolved" : "resolved, unwind metadata not confirmed"));

    SetAobTargetHealth(AobTargetId::SetIntensity,
        addrSetIntensity
            ? (IsValidatedCallTarget(addrSetIntensity) ? RuntimeHealthState::Ready : RuntimeHealthState::Degraded)
            : RuntimeHealthState::Disabled,
        addrSetIntensity,
        !addrSetIntensity ? "Tick+0x2CC unresolved"
            : (IsValidatedCallTarget(addrSetIntensity) ? "Tick+0x2CC resolved" : "resolved, unwind metadata not confirmed"));

    const bool cloudPackInstalled = addrCloudPack && g_pOrigCloudPack;
    RuntimeHealthState cloudPackState = RuntimeHealthState::Disabled;
    const char* cloudPackNote = "unresolved or hook failed";
    if (cloudPackInstalled) {
        if (cloudPackFallbackUsed || cloudPackValidationMismatch) {
            cloudPackState = RuntimeHealthState::Degraded;
            cloudPackNote = cloudPackFallbackUsed
                ? "fallback pattern + hook installed"
                : "derived target + call-pair validation mismatch";
        } else {
            cloudPackState = RuntimeHealthState::Ready;
            cloudPackNote = "call-pair derived + hook installed";
        }
    }
    SetAobTargetHealth(AobTargetId::CloudPack, cloudPackState, addrCloudPack, cloudPackNote);

    const bool windPackInstalled = addrWindPack && g_pOrigWindPack;
    SetAobTargetHealth(AobTargetId::WindPack,
        windPackInstalled
            ? (windPackFallbackUsed ? RuntimeHealthState::Degraded : RuntimeHealthState::Ready)
            : RuntimeHealthState::Disabled,
        addrWindPack,
        !windPackInstalled ? "unresolved or hook failed"
            : (windPackFallbackUsed ? "fallback pattern + hook installed" : "derived from CloudPack caller + hook installed"));

    SetAobTargetHealth(AobTargetId::PostProcessLayerUpdate,
        (addrPPLayerUpdate && g_pOrigPPLayerUpdate)
            ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        addrPPLayerUpdate,
        (addrPPLayerUpdate && g_pOrigPPLayerUpdate) ? "optional diag hook installed" : "optional diag hook unavailable");

    SetAobTargetHealth(AobTargetId::GetLayerMeta,
        addrGetLayerMeta
            ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        addrGetLayerMeta,
        addrGetLayerMeta ? "optional diagnostic resolver ready" : "optional diagnostic resolver unavailable");

    const bool weatherFrameInstalled = addrWeatherFrameUpdate && g_pOrigWeatherFrameUpdate;
    SetAobTargetHealth(AobTargetId::WeatherFrameUpdate,
        weatherFrameInstalled
            ? ((weatherFrameForcedUsed || weatherFramePatternFallbackUsed) ? RuntimeHealthState::Degraded : RuntimeHealthState::Ready)
            : RuntimeHealthState::Disabled,
        addrWeatherFrameUpdate,
        !weatherFrameInstalled ? "fog frame hook unavailable"
            : (weatherFrameForcedUsed ? "forced xref delta fallback + hook installed"
               : (weatherFramePatternFallbackUsed ? "pattern fallback + hook installed" : "xref/unwind validated + hook installed")));

    const bool fogInstalled = addrAtmosFogBlend && g_pOrigAtmosFogBlend;
    SetAobTargetHealth(AobTargetId::AtmosFogBlend,
        fogInstalled
            ? ((fogForcedUsed || fogPatternFallbackUsed) ? RuntimeHealthState::Degraded : RuntimeHealthState::Ready)
            : RuntimeHealthState::Disabled,
        addrAtmosFogBlend,
        !fogInstalled ? "direct fog hook unavailable"
            : (fogForcedUsed ? "forced prior-call fallback + hook installed"
               : (fogPatternFallbackUsed ? "pattern fallback + hook installed" : "xref/prologue validated + hook installed")));

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
        (timeReady && g_addrTimeDebugHandler) ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        g_addrTimeDebugHandler,
        (timeReady && g_addrTimeDebugHandler) ? "debug handler + vtable offsets ready" : "time debug handler unresolved");

    SetAobTargetHealth(AobTargetId::NativeToast,
        nativeToastReady ? RuntimeHealthState::Ready : RuntimeHealthState::Disabled,
        reinterpret_cast<uintptr_t>(g_pNativeToastRootGlobal),
        nativeToastReady ? "native toast bridge ready" : "native toast bridge unavailable");

    RecomputeRuntimeHealthSummary();
    LogRuntimeHealthSummary();
    return true;
}


