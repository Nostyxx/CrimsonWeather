#include "pch.h"

#include "sky_image_loader.h"
#include "sky_texture_override.h"
#include "runtime_shared.h"

#include <d3d12.h>
#include <dxgi.h>

#include <filesystem>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if defined(CW_VERBOSE_SKY_TEXTURE) || defined(CW_DEV_BUILD)
#define CW_SKY_VERBOSE_LOG(...) Log(__VA_ARGS__)
#else
#define CW_SKY_VERBOSE_LOG(...) do {} while (0)
#endif

#if defined(CW_WIND_ONLY)

bool InitializeSkyTextureOverride(HMODULE) { return true; }
void SkyTextureOnInitDevice(ID3D12Device*) {}
void SkyTextureOnPresent() {}
void ShutdownSkyTextureOverride() {}
void MoonTextureReload() {}
const char* MoonTextureStatus() { return "unavailable in Wind only"; }
bool MoonTextureReady() { return false; }
void MoonTextureRefreshList() {}
int MoonTextureOptionCount() { return 1; }
const char* MoonTextureOptionName(int) { return "Native"; }
const char* MoonTextureOptionLabel(int) { return "Native"; }
const char* MoonTextureOptionPack(int) { return ""; }
bool MoonTextureOptionIsAnimated(int) { return false; }
int MoonTextureFindOptionByName(const char*) { return 0; }
int MoonTextureSelectedOption() { return 0; }
void MoonTextureSelectOption(int) {}
bool MoonTextureSelectByName(const char*) { return false; }
void MilkywayTextureReload() {}
const char* MilkywayTextureStatus() { return "unavailable in Wind only"; }
bool MilkywayTextureReady() { return false; }
void MilkywayTextureRefreshList() {}
int MilkywayTextureOptionCount() { return 1; }
const char* MilkywayTextureOptionName(int) { return "Native"; }
const char* MilkywayTextureOptionLabel(int) { return "Native"; }
const char* MilkywayTextureOptionPack(int) { return ""; }
bool MilkywayTextureOptionIsAnimated(int) { return false; }
int MilkywayTextureFindOptionByName(const char*) { return 0; }
int MilkywayTextureSelectedOption() { return 0; }
void MilkywayTextureSelectOption(int) {}
bool MilkywayTextureSelectByName(const char*) { return false; }

#else

namespace {

struct TextureOption {
    std::string name;
    std::string label;
    std::string pack;
    std::string path;
    bool animated = false;
};

HMODULE g_module = nullptr;
SRWLOCK g_stateLock = SRWLOCK_INIT;

struct StateLockGuard {
    StateLockGuard() {
        AcquireSRWLockExclusive(&g_stateLock);
    }

    ~StateLockGuard() {
        ReleaseSRWLockExclusive(&g_stateLock);
    }

    StateLockGuard(const StateLockGuard&) = delete;
    StateLockGuard& operator=(const StateLockGuard&) = delete;
};

// UI state uses g_stateLock. D3D12 hook state uses g_hookLock.
// If a path ever needs both, take g_stateLock first, then g_hookLock.
std::atomic<bool> g_skyTextureHooksDisabled{ false };

struct SrvBinding {
    ID3D12Resource* nativeResource = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dest = {};
    D3D12_SHADER_RESOURCE_VIEW_DESC nativeDesc = {};
    bool hasDesc = false;
    bool copied = false;
};

struct CachedTexture {
    std::string path;
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT mips = 0;
    UINT width = 0;
    UINT height = 0;
};

enum class AnimationLoopMode {
    Forward,
    PingPong,
    Hold,
    Once
};

constexpr size_t kDefaultAnimatedTextureGpuSlotCount = 12;
constexpr size_t kMinAnimatedTextureGpuSlotCount = 4;
constexpr size_t kMaxAnimatedTextureGpuSlotCount = 120;

struct AnimatedFrameSlot {
    size_t frameIndex = static_cast<size_t>(-1);
    ID3D12Resource* resource = nullptr;
    ULONGLONG lastUsedTick = 0;
    UINT64 gpuEstimateBytes = 0;
    UINT64 sourceBytes = 0;
    DWORD loadMs = 0;
};

struct CachedAnimation {
    std::string path;
    std::vector<std::string> framePaths;
    std::vector<AnimatedFrameSlot> frameSlots;
    std::vector<double> frameDurationsMs;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT mips = 0;
    UINT width = 0;
    UINT height = 0;
    double frameMs = 83.3333333333;
    AnimationLoopMode loopMode = AnimationLoopMode::Forward;
    size_t startFrame = 0;
    bool randomStart = false;
};

void ReleaseAnimationResources(CachedAnimation& cached);
double BytesToMiB(UINT64 bytes);

size_t ConfiguredAnimatedTextureGpuSlotCount() {
    return std::clamp<size_t>(
        static_cast<size_t>(std::max(0, g_cfg.textureSwitcherAnimatedTextureGpuSlots)),
        kMinAnimatedTextureGpuSlotCount,
        kMaxAnimatedTextureGpuSlotCount);
}

size_t AnimationGpuSlotCapacity(const CachedAnimation& cached) {
    return cached.frameSlots.empty() ? ConfiguredAnimatedTextureGpuSlotCount() : cached.frameSlots.size();
}

void EnsureAnimationSlotStorage(CachedAnimation& cached) {
    if (cached.frameSlots.empty()) {
        cached.frameSlots.assign(ConfiguredAnimatedTextureGpuSlotCount(), AnimatedFrameSlot{});
    }
}

struct TextureSlot;

struct AnimationLoadRequest {
    TextureSlot* slot = nullptr;
    std::string path;
    size_t frameIndex = 0;
    uint32_t priority = 0;
    ULONGLONG queuedTick = 0;
};

struct TextureFingerprintTarget {
    const char* name = "";
    const char* pattern = "";
    uintptr_t start = 0;
    size_t range = 0;
};

struct TextureSlot {
    const char* displayName = "";
    const char* logTag = "";
    const char* uiLogTag = "";
    const char* subdir = "";
    const char* fileName = "";
    UINT nativeWidth = 0;
    UINT nativeHeight = 0;
    UINT nativeMips = 0;
    int fingerprintHitsRequired = 0;
    size_t maxBindings = 0;
    bool useProof = false;
    bool lockFirstNative = false;
    bool rewriteAllOnCopy = false;
    bool requireContentProof = false;
    uint64_t expectedTopMipHash = 0;
    TextureFingerprintTarget* fingerprintTargets = nullptr;
    size_t fingerprintTargetCount = 0;
    bool fingerprintResolved = false;

    std::vector<ID3D12Resource*> nativeResources;
    ID3D12Resource* primaryNativeResource = nullptr;
    std::atomic<bool> nativeLocked{ false };
    std::vector<SrvBinding> bindings;
    std::vector<SrvBinding> pendingBindings;

    std::string selectedPath;
    bool selectedAnimated = false;
    std::string activePath;
    bool activeAnimated = false;
    std::string lastMissingWarning;
    DXGI_FORMAT fileFormat = DXGI_FORMAT_UNKNOWN;
    UINT fileMips = 0;
    UINT fileWidth = 0;
    UINT fileHeight = 0;
    std::atomic<ID3D12Resource*> fileResource{ nullptr };
    std::vector<CachedTexture> cache;
    std::vector<CachedAnimation> animCache;
    size_t animFrameIndex = 0;
    size_t animStartFrame = 0;
    int animDirection = 1;
    int animStartDirection = 1;
    ULONGLONG animStartTick = 0;
    ULONGLONG animLastTick = 0;
    bool animFrameApplied = false;
    uint64_t animFrameLoadCount = 0;
    uint64_t animFrameMissCount = 0;
    uint64_t animFrameEvictCount = 0;
    uint64_t animRewriteCount = 0;
    uint64_t animFrameQueueCount = 0;
    uint64_t animFrameSkipCount = 0;
    uint64_t animFrameStaleDropCount = 0;
    bool animQueuedWindowValid = false;
    size_t animQueuedWindowFrame = static_cast<size_t>(-1);
    int animQueuedWindowDirection = 1;
    UINT64 animResidentGpuEstimateBytes = 0;
    UINT64 animPeakResidentGpuEstimateBytes = 0;
    DWORD animLastLoadMs = 0;
    DWORD animPeakLoadMs = 0;
    ULONGLONG animLastDiagnosticTick = 0;
    uint32_t animDiagnosticLogCount = 0;

    bool sawUnormSrv = false;
    bool sawSrgbSrv = false;
    uint32_t sourceSrvCount = 0;
    uint32_t copiedSrvCount = 0;
    int lastProofLevel = -99;
    bool contentProofHit = false;
    std::atomic<uint32_t> contentRejectLogCount{ 0 };

    std::vector<TextureOption> options;
    int selectedOption = 0;
    bool optionsScanned = false;
    char status[192] = "";

    std::atomic<uint32_t> candidateLogCount{ 0 };
    std::atomic<uint32_t> rejectLogCount{ 0 };
    std::atomic<uint32_t> fingerprintRejectCount{ 0 };
    std::atomic<uint32_t> descriptorCopyLogCount{ 0 };
    std::atomic<uint32_t> nativeLogCount{ 0 };
    std::atomic<uint32_t> descriptorLogCount{ 0 };
    std::atomic<uint32_t> pendingDescriptorLogCount{ 0 };
    std::atomic<bool> fingerprintSummaryLogged{ false };

    TextureSlot(
        const char* displayNameIn,
        const char* logTagIn,
        const char* uiLogTagIn,
        const char* subdirIn,
        const char* fileNameIn,
        UINT nativeWidthIn,
        UINT nativeHeightIn,
        UINT nativeMipsIn,
        int fingerprintHitsRequiredIn,
        size_t maxBindingsIn,
        bool useProofIn,
        bool lockFirstNativeIn,
        bool rewriteAllOnCopyIn,
        bool requireContentProofIn,
        uint64_t expectedTopMipHashIn,
        TextureFingerprintTarget* fingerprintTargetsIn,
        size_t fingerprintTargetCountIn,
        const char* initialStatus)
        : displayName(displayNameIn),
          logTag(logTagIn),
          uiLogTag(uiLogTagIn),
          subdir(subdirIn),
          fileName(fileNameIn),
          nativeWidth(nativeWidthIn),
          nativeHeight(nativeHeightIn),
          nativeMips(nativeMipsIn),
          fingerprintHitsRequired(fingerprintHitsRequiredIn),
          maxBindings(maxBindingsIn),
          useProof(useProofIn),
          lockFirstNative(lockFirstNativeIn),
          rewriteAllOnCopy(rewriteAllOnCopyIn),
          requireContentProof(requireContentProofIn),
          expectedTopMipHash(expectedTopMipHashIn),
          fingerprintTargets(fingerprintTargetsIn),
          fingerprintTargetCount(fingerprintTargetCountIn) {
        strcpy_s(status, initialStatus ? initialStatus : "");
    }
};

int RewriteTrackedDescriptorsLocked(TextureSlot& slot, ID3D12Resource* replacement);

using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PFN_CreateCommandList = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);
using PFN_CreateShaderResourceView = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CopyDescriptors = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CopyDescriptorsSimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CreateCommittedResource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreateCommittedResource1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CreateCommittedResource2 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CreateCommittedResource3 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**);
using PFN_CopyTextureRegion = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);

PFN_D3D12CreateDevice g_realD3D12CreateDevice = nullptr;
PFN_CreateCommandList g_realCreateCommandList = nullptr;
PFN_CreateShaderResourceView g_realCreateShaderResourceView = nullptr;
PFN_CopyDescriptors g_realCopyDescriptors = nullptr;
PFN_CopyDescriptorsSimple g_realCopyDescriptorsSimple = nullptr;
PFN_CreateCommittedResource g_realCreateCommittedResource = nullptr;
PFN_CreateCommittedResource1 g_realCreateCommittedResource1 = nullptr;
PFN_CreateCommittedResource2 g_realCreateCommittedResource2 = nullptr;
PFN_CreateCommittedResource3 g_realCreateCommittedResource3 = nullptr;
PFN_CopyTextureRegion g_realCopyTextureRegion = nullptr;

CRITICAL_SECTION g_hookLock;
std::atomic<bool> g_hookLockReady{ false };
std::atomic<bool> g_hookShuttingDown{ false };
std::atomic<bool> g_hookThreadStarted{ false };
std::atomic<bool> g_d3d12CreateDeviceHooked{ false };
std::atomic<bool> g_deviceHooksInstalled{ false };
std::atomic<bool> g_commandListHooksInstalled{ false };
std::atomic<ID3D12Device*> g_device{ nullptr };
std::atomic<ID3D12CommandQueue*> g_uploadQueue{ nullptr };
void* g_d3d12CreateDeviceTarget = nullptr;
HANDLE g_animationWorkerThread = nullptr;
HANDLE g_animationWorkerEvent = nullptr;
std::atomic<bool> g_animationWorkerStop{ false };
std::vector<AnimationLoadRequest> g_animationLoadRequests;
TextureFingerprintTarget g_moonFingerprintTargets[] = {
    {
        "MoonTextureNodeApply",
        "40 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 78 "
        "48 8B FA 4C 8B F1 8B 81 60 01 00 00 45 32 E4 "
        "44 88 A4 24 C0 00 00 00 48 8B D1 48 8D 8C 24 C8 00 00 00 "
        "E8 ?? ?? ?? ?? 90 65 48 8B 04 25 58 00 00 00",
        0,
        0x500
    },
    {
        "MoonTextureWorkerUpdate",
        "40 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 38 "
        "48 8B E9 40 32 FF 40 88 BC 24 88 00 00 00 "
        "0F B6 81 08 03 00 00 88 84 24 90 00 00 00 8B 99 20 03 00 00",
        0,
        0x800
    },
    {
        "MoonTextureWorkerLoop",
        "48 89 5C 24 20 56 57 41 56 48 83 EC 20 48 8B F9 "
        "48 8D 4C 24 50 E8 ?? ?? ?? ?? 44 0F B6 B7 08 03 00 00 "
        "41 80 FE FF 0F 84 ?? ?? ?? ?? 48 8B 4F 10 B2 03 E8",
        0,
        0x300
    },
    {
        "MoonTextureThreadStart",
        "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 40 "
        "65 48 8B 04 25 58 00 00 00 48 8B F9 B9 70 02 00 00 "
        "48 8B 10 48 8B 07 48 89 04 11 48 8D 4C 24 50 E8 ?? ?? ?? ??",
        0,
        0x200
    },
};
TextureFingerprintTarget g_milkywayFingerprintTargets[] = {
    {
        "MilkywayTextureUploadCommit",
        "49 8B 45 00 48 8B 80 D8 00 00 00 48 89 45 D7 "
        "48 8B 40 10 48 89 55 AF 48 8B 08 E8 ?? ?? ?? ?? "
        "48 8B D8 48 89 45 AF",
        0,
        0x220
    },
    {
        "MilkywayTextureLoaderCheck",
        "30 48 8D 54 24 60 44 88 7C 24 28 4D 8B CE 44 8B C0 "
        "4C 89 7C 24 20 48 8B CF 8B E8 E8 ?? ?? ?? ?? "
        "0F B6 D8 3C 01 75 28 4C 39 3E 74 23",
        0,
        0x220
    },
    {
        "MilkywayTextureNodeApply",
        "0F B7 44 24 70 66 89 83 AC 00 00 00 48 8B 03 4C 8B CD "
        "4D 8B C7 49 8B D6 48 8B CB FF 90 28 01 00 00 80 A3 AF 00 00 00 D3",
        0,
        0x240
    },
    {
        "MilkywayTextureNodeUpdate",
        "4C 8D 44 24 30 4D 85 F6 4D 0F 45 C6 66 89 5C 24 20 "
        "4D 8B CF 48 8B D6 48 8B CD E8 ?? ?? ?? ?? 0F B6 D8 84 C0 75 30",
        0,
        0x240
    },
    {
        "MilkywayTextureResourceAttach",
        "44 88 65 69 0F B7 45 68 66 89 44 24 20 4C 8B 4D 60 "
        "45 33 C0 48 8D 55 C0 48 8B CF E8 ?? ?? ?? ?? 84 C0 74 49",
        0,
        0x240
    },
};

TextureSlot g_moonSlot(
    "Moon texture",
    "moon-main",
    "moon-ui",
    "moon",
    "moon",
    512,
    512,
    10,
    3,
    16,
    true,
    true,
    true,
    true,
    0x3516A1104AF9BA5Cull,
    g_moonFingerprintTargets,
    sizeof(g_moonFingerprintTargets) / sizeof(g_moonFingerprintTargets[0]),
    "Moon texture: integrated hook waiting");

TextureSlot g_milkywaySlot(
    "Milky Way texture",
    "milkyway",
    "milkyway-ui",
    "milkyway",
    "milkyway",
    2048,
    1024,
    1,
    2,
    24,
    false,
    false,
    false,
    true,
    0x4F08797486280FDAull,
    g_milkywayFingerprintTargets,
    sizeof(g_milkywayFingerprintTargets) / sizeof(g_milkywayFingerprintTargets[0]),
    "Milky Way texture: integrated hook waiting");

TextureSlot* g_textureSlots[] = { &g_moonSlot, &g_milkywaySlot };

constexpr const char* kNoMoonOptionName = "No Moon";
constexpr const char* kNoMilkywayOptionName = "No Milky Way";
constexpr const char* kNoMoonSentinelPath = "cw://builtin/no-moon";
constexpr const char* kNoMilkywaySentinelPath = "cw://builtin/no-milkyway";

bool StringEqualsIgnoreCase(const std::string& a, const std::string& b);
bool StringEqualsIgnoreCase(const std::string& a, const char* b);

bool TextureSwitcherEnabled() {
    return g_cfg.textureSwitcherEnabled;
}

bool IsBlankTexturePath(const std::string& path) {
    return StringEqualsIgnoreCase(path, kNoMoonSentinelPath) ||
           StringEqualsIgnoreCase(path, kNoMilkywaySentinelPath);
}

const char* BlankTextureOptionName(const TextureSlot& slot) {
    return &slot == &g_moonSlot ? kNoMoonOptionName : kNoMilkywayOptionName;
}

const char* BlankTextureSentinelPath(const TextureSlot& slot) {
    return &slot == &g_moonSlot ? kNoMoonSentinelPath : kNoMilkywaySentinelPath;
}

void BuildBlankSkyImage(SkyImageData& image) {
    image.pixels.assign(4, 0);
    image.mipLevels.clear();
    image.mipLevels.push_back({ 1, 1, 1, 4, 0, 4 });
    image.width = 1;
    image.height = 1;
    image.mips = 1;
    image.sourceRows = 1;
    image.sourceRowPitch = 4;
    image.format = DXGI_FORMAT_R8G8B8A8_UNORM;
}

void SetTextureStatus(TextureSlot& slot, const char* fmt, ...) {
    char status[sizeof(slot.status)] = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(status, sizeof(status), fmt, args);
    va_end(args);

    const bool lockReady = g_hookLockReady.load();
    if (lockReady) {
        EnterCriticalSection(&g_hookLock);
    }
    strcpy_s(slot.status, status);
    if (lockReady) {
        LeaveCriticalSection(&g_hookLock);
    }
}

void CopyTextureStatus(TextureSlot& slot, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    const bool lockReady = g_hookLockReady.load();
    if (lockReady) {
        EnterCriticalSection(&g_hookLock);
    }
    strcpy_s(out, outSize, slot.status);
    if (lockReady) {
        LeaveCriticalSection(&g_hookLock);
    }
}

std::string ModuleDirectory() {
    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(g_module, modulePath, MAX_PATH);
    char* slash = strrchr(modulePath, '\\');
    if (slash) {
        slash[1] = '\0';
        return modulePath;
    }
    return {};
}

std::string TextureRootDirectory(const TextureSlot& slot) {
    return ModuleDirectory() + "CrimsonWeather\\" + slot.subdir;
}

bool StringEqualsIgnoreCase(const std::string& a, const std::string& b) {
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

bool StringEqualsIgnoreCase(const std::string& a, const char* b) {
    return b && _stricmp(a.c_str(), b) == 0;
}

size_t RemoveAnimationLoadRequestsLocked(TextureSlot& slot, const char* path) {
    const size_t before = g_animationLoadRequests.size();
    g_animationLoadRequests.erase(
        std::remove_if(g_animationLoadRequests.begin(), g_animationLoadRequests.end(),
            [&](const AnimationLoadRequest& request) {
                return request.slot == &slot &&
                    (!path || !path[0] || StringEqualsIgnoreCase(request.path, path));
            }),
        g_animationLoadRequests.end());
    return before - g_animationLoadRequests.size();
}

int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseBytePattern(const char* pattern, std::vector<uint8_t>& bytes, std::vector<uint8_t>& mask) {
    bytes.clear();
    mask.clear();
    if (!pattern) {
        return false;
    }

    const char* p = pattern;
    while (*p) {
        while (*p == ' ') {
            ++p;
        }
        if (!*p) {
            break;
        }

        if (p[0] == '?' && p[1] == '?') {
            bytes.push_back(0);
            mask.push_back(0);
            p += 2;
            continue;
        }

        const int hi = HexNibble(p[0]);
        const int lo = HexNibble(p[1]);
        if (hi < 0 || lo < 0) {
            return false;
        }

        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
        mask.push_back(0xFF);
        p += 2;
    }

    return !bytes.empty() && bytes.size() == mask.size();
}

uintptr_t ScanExecutableModulePattern(const char* pattern) {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> mask;
    if (!ParseBytePattern(pattern, bytes, mask)) {
        return 0;
    }

    auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    if (!module) {
        return 0;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    auto* section = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }

        const size_t size = section->Misc.VirtualSize;
        if (size < bytes.size()) {
            continue;
        }

        const uintptr_t base = reinterpret_cast<uintptr_t>(module) + section->VirtualAddress;
        auto* mem = reinterpret_cast<const uint8_t*>(base);
        for (size_t j = 0; j <= size - bytes.size(); ++j) {
            bool match = true;
            for (size_t k = 0; k < bytes.size(); ++k) {
                if (mask[k] && mem[j + k] != bytes[k]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                return base + j;
            }
        }
    }

    return 0;
}

bool ResolveFingerprintTargets(TextureSlot& slot) {
    if (slot.fingerprintResolved) {
        return true;
    }

    HMODULE exe = GetModuleHandleA(nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
    bool allFound = base != 0;
    for (size_t i = 0; i < slot.fingerprintTargetCount; ++i) {
        TextureFingerprintTarget& target = slot.fingerprintTargets[i];
        target.start = ScanExecutableModulePattern(target.pattern);
        if (target.start) {
            CW_SKY_VERBOSE_LOG("[%s] fingerprint %s = %p rva=0x%llX range=0x%zX\n",
                slot.logTag,
                target.name,
                reinterpret_cast<void*>(target.start),
                static_cast<unsigned long long>(target.start - base),
                target.range);
        } else {
            allFound = false;
            Log("[W] %s fingerprint %s AOB not found\n", slot.logTag, target.name);
        }
    }

    slot.fingerprintResolved = allFound;
    return allFound;
}

void LogFingerprintSummary(TextureSlot& slot) {
    bool expected = false;
    if (!slot.fingerprintSummaryLogged.compare_exchange_strong(expected, true)) {
        return;
    }

    HMODULE exe = GetModuleHandleA(nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
    int ready = 0;
    for (size_t i = 0; i < slot.fingerprintTargetCount; ++i) {
        if (slot.fingerprintTargets[i].start) {
            ++ready;
        }
    }

    Log("[%s] fingerprint summary ready=%d/%zu\n", slot.logTag, ready, slot.fingerprintTargetCount);
    for (size_t i = 0; i < slot.fingerprintTargetCount; ++i) {
        const TextureFingerprintTarget& target = slot.fingerprintTargets[i];
        if (target.start && base) {
            CW_SKY_VERBOSE_LOG("[%s] fingerprint target %s rva=0x%llX range=0x%llX\n",
                slot.logTag,
                target.name,
                static_cast<unsigned long long>(target.start - base),
                static_cast<unsigned long long>(target.range));
        } else {
            Log("[W] %s fingerprint target %s unresolved\n", slot.logTag, target.name);
        }
    }
}

bool IsTextureFileName(const TextureSlot& slot, const std::string& name) {
    const std::string dds = std::string(slot.fileName) + ".dds";
    const std::string png = std::string(slot.fileName) + ".png";
    return StringEqualsIgnoreCase(name, dds) || StringEqualsIgnoreCase(name, png);
}

constexpr double kDefaultAnimatedMoonFps = 12.0;
constexpr long kMaxAnimatedMoonManifestBytes = 1024 * 1024;

bool IsRegularFilePath(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

bool ReadSmallTextFile(const std::filesystem::path& path, std::string& outText) {
    outText.clear();
    FILE* f = nullptr;
    fopen_s(&f, path.string().c_str(), "rb");
    if (!f) {
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > kMaxAnimatedMoonManifestBytes) {
        fclose(f);
        return false;
    }
    outText.resize(static_cast<size_t>(size));
    const size_t read = outText.empty() ? 0 : fread(outText.data(), 1, outText.size(), f);
    fclose(f);
    return read == outText.size();
}

void StripJsonCommentsInPlace(std::string& json) {
    bool inString = false;
    bool escaped = false;
    std::string out;
    out.reserve(json.size());
    for (size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (inString) {
            out.push_back(c);
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
            out.push_back(c);
            continue;
        }
        if (c == '/' && i + 1 < json.size() && json[i + 1] == '/') {
            while (i < json.size() && json[i] != '\n' && json[i] != '\r') {
                ++i;
            }
            if (i < json.size()) {
                out.push_back(json[i]);
            }
            continue;
        }
        if (c == '/' && i + 1 < json.size() && json[i + 1] == '*') {
            i += 2;
            while (i + 1 < json.size() && !(json[i] == '*' && json[i + 1] == '/')) {
                if (json[i] == '\n' || json[i] == '\r') {
                    out.push_back(json[i]);
                }
                ++i;
            }
            if (i + 1 < json.size()) {
                ++i;
            }
            continue;
        }
        out.push_back(c);
    }
    json.swap(out);
}

size_t FindJsonValueStart(const std::string& json, const char* key) {
    const std::string quotedKey = std::string("\"") + key + "\"";
    const size_t keyPos = json.find(quotedKey);
    if (keyPos == std::string::npos) {
        return std::string::npos;
    }
    const size_t colon = json.find(':', keyPos + quotedKey.size());
    if (colon == std::string::npos) {
        return std::string::npos;
    }
    size_t value = colon + 1;
    while (value < json.size() && isspace(static_cast<unsigned char>(json[value]))) {
        ++value;
    }
    return value;
}

bool ExtractJsonString(const std::string& json, const char* key, std::string& outValue) {
    outValue.clear();
    size_t value = FindJsonValueStart(json, key);
    if (value == std::string::npos || value >= json.size() || json[value] != '"') {
        return false;
    }
    ++value;
    std::string result;
    while (value < json.size()) {
        const char c = json[value++];
        if (c == '"') {
            outValue = result;
            return true;
        }
        if (c == '\\' && value < json.size()) {
            result.push_back(json[value++]);
        } else {
            result.push_back(c);
        }
    }
    return false;
}

bool ExtractJsonInt(const std::string& json, const char* key, int& outValue) {
    size_t value = FindJsonValueStart(json, key);
    if (value == std::string::npos) {
        return false;
    }
    char* end = nullptr;
    const long parsed = strtol(json.c_str() + value, &end, 10);
    if (end == json.c_str() + value || parsed < 0 || parsed > INT_MAX) {
        return false;
    }
    outValue = static_cast<int>(parsed);
    return true;
}

bool ExtractJsonDouble(const std::string& json, const char* key, double& outValue) {
    size_t value = FindJsonValueStart(json, key);
    if (value == std::string::npos) {
        return false;
    }
    char* end = nullptr;
    const double parsed = strtod(json.c_str() + value, &end);
    if (end == json.c_str() + value || !std::isfinite(parsed)) {
        return false;
    }
    outValue = parsed;
    return true;
}

bool ExtractJsonBool(const std::string& json, const char* key, bool& outValue) {
    size_t value = FindJsonValueStart(json, key);
    if (value == std::string::npos) {
        return true;
    }
    while (value < json.size() && isspace(static_cast<unsigned char>(json[value]))) {
        ++value;
    }
    if (value + 4 <= json.size() && _strnicmp(json.c_str() + value, "true", 4) == 0) {
        outValue = true;
        return true;
    }
    if (value + 5 <= json.size() && _strnicmp(json.c_str() + value, "false", 5) == 0) {
        outValue = false;
        return true;
    }
    return false;
}

bool ExtractJsonDoubleArray(const std::string& json, const char* key, std::vector<double>& outValues, bool& outPresent) {
    outValues.clear();
    outPresent = false;
    size_t value = FindJsonValueStart(json, key);
    if (value == std::string::npos) {
        return true;
    }
    outPresent = true;
    if (value >= json.size() || json[value] != '[') {
        return false;
    }
    ++value;
    while (value < json.size()) {
        while (value < json.size() && isspace(static_cast<unsigned char>(json[value]))) {
            ++value;
        }
        if (value < json.size() && json[value] == ']') {
            return true;
        }

        char* end = nullptr;
        const double parsed = strtod(json.c_str() + value, &end);
        if (end == json.c_str() + value || !std::isfinite(parsed) || parsed <= 0.0) {
            return false;
        }
        outValues.push_back(parsed);
        value = static_cast<size_t>(end - json.c_str());

        while (value < json.size() && isspace(static_cast<unsigned char>(json[value]))) {
            ++value;
        }
        if (value < json.size() && json[value] == ',') {
            ++value;
            continue;
        }
        if (value < json.size() && json[value] == ']') {
            return true;
        }
        return false;
    }
    return false;
}

bool BuildFramePath(const std::filesystem::path& folder,
                    const std::string& pattern,
                    int index,
                    std::filesystem::path& outPath) {
    if (pattern.empty() || pattern.find('%') == std::string::npos) {
        return false;
    }
    char fileName[260] = {};
    const int written = sprintf_s(fileName, sizeof(fileName), pattern.c_str(), index);
    if (written <= 0 || written >= static_cast<int>(sizeof(fileName))) {
        return false;
    }
    outPath = folder / fileName;
    return true;
}

bool ParseAnimationLoopMode(const std::string& text, AnimationLoopMode& outMode) {
    if (text.empty() || _stricmp(text.c_str(), "forward") == 0 || _stricmp(text.c_str(), "loop") == 0) {
        outMode = AnimationLoopMode::Forward;
        return true;
    }
    if (_stricmp(text.c_str(), "pingpong") == 0 ||
        _stricmp(text.c_str(), "ping-pong") == 0 ||
        _stricmp(text.c_str(), "bounce") == 0 ||
        _stricmp(text.c_str(), "reverse") == 0) {
        outMode = AnimationLoopMode::PingPong;
        return true;
    }
    if (_stricmp(text.c_str(), "hold") == 0) {
        outMode = AnimationLoopMode::Hold;
        return true;
    }
    if (_stricmp(text.c_str(), "once") == 0 ||
        _stricmp(text.c_str(), "onceHoldLast") == 0 ||
        _stricmp(text.c_str(), "once-hold-last") == 0) {
        outMode = AnimationLoopMode::Once;
        return true;
    }
    return false;
}

const char* AnimationLoopModeName(AnimationLoopMode mode) {
    switch (mode) {
    case AnimationLoopMode::PingPong: return "pingpong";
    case AnimationLoopMode::Hold: return "hold";
    case AnimationLoopMode::Once: return "once";
    case AnimationLoopMode::Forward:
    default:
        return "forward";
    }
}

bool LoadAnimatedTextureFrameList(const std::filesystem::path& folder,
                                  std::vector<std::string>& outFrames,
                                  double& outFps,
                                  std::vector<double>& outFrameDurationsMs,
                                  AnimationLoopMode& outLoopMode,
                                  size_t& outStartFrame,
                                  bool& outRandomStart,
                                  bool allowFallback) {
    outFrames.clear();
    outFrameDurationsMs.clear();
    outFps = kDefaultAnimatedMoonFps;
    outLoopMode = AnimationLoopMode::Forward;
    outStartFrame = 0;
    outRandomStart = false;

    const std::filesystem::path manifestPath = folder / "manifest.json";
    if (IsRegularFilePath(manifestPath)) {
        std::string json;
        std::string pattern;
        std::string loopMode;
        std::vector<double> frameDurations;
        int frameCount = 0;
        int startFrame = 0;
        double fps = kDefaultAnimatedMoonFps;
        if (!ReadSmallTextFile(manifestPath, json)) {
            return false;
        }
        StripJsonCommentsInPlace(json);
        if (!ExtractJsonString(json, "framePattern", pattern) ||
            !ExtractJsonInt(json, "frames", frameCount)) {
            return false;
        }
        ExtractJsonDouble(json, "fps", fps);
        ExtractJsonInt(json, "startFrame", startFrame);
        if (!ExtractJsonBool(json, "randomStart", outRandomStart)) {
            return false;
        }
        if (ExtractJsonString(json, "loopMode", loopMode) || ExtractJsonString(json, "loop", loopMode)) {
            if (!ParseAnimationLoopMode(loopMode, outLoopMode)) {
                return false;
            }
        }
        if (frameCount < 2 || !std::isfinite(fps) || fps <= 0.0) {
            return false;
        }
        if (startFrame < 0 || startFrame >= frameCount) {
            return false;
        }
        bool durationsPresent = false;
        if (!ExtractJsonDoubleArray(json, "frameDurations", frameDurations, durationsPresent)) {
            return false;
        }
        if (durationsPresent && frameDurations.size() != static_cast<size_t>(frameCount)) {
            return false;
        }
        for (int i = 0; i < frameCount; ++i) {
            std::filesystem::path framePath;
            if (!BuildFramePath(folder, pattern, i, framePath) || !IsRegularFilePath(framePath)) {
                return false;
            }
            outFrames.push_back(framePath.string());
        }
        outFps = fps;
        outFrameDurationsMs = std::move(frameDurations);
        outStartFrame = static_cast<size_t>(startFrame);
        return true;
    }

    if (!allowFallback) {
        return false;
    }

    const char* exts[] = { ".png", ".dds" };
    for (const char* ext : exts) {
        std::vector<std::string> frames;
        for (int i = 0; i < INT_MAX; ++i) {
            char fileName[32] = {};
            sprintf_s(fileName, "frame_%03d%s", i, ext);
            const std::filesystem::path framePath = folder / fileName;
            if (!IsRegularFilePath(framePath)) {
                break;
            }
            frames.push_back(framePath.string());
        }
        if (frames.size() >= 2) {
            outFrames = std::move(frames);
            outFps = kDefaultAnimatedMoonFps;
            outFrameDurationsMs.clear();
            outLoopMode = AnimationLoopMode::Forward;
            outStartFrame = 0;
            outRandomStart = false;
            return true;
        }
    }
    return false;
}

bool TryAnimatedTexturePackPath(const std::filesystem::path& root,
                                const std::filesystem::path& folder,
                                std::string& outPack,
                                std::string& outLabel) {
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(folder, root, ec);
    std::vector<std::string> parts;
    if (!ec) {
        for (const std::filesystem::path& part : relative) {
            parts.push_back(part.string());
        }
    }
    if (parts.size() != 2 || parts[0].empty() || parts[1].empty()) {
        return false;
    }
    std::vector<std::string> frames;
    std::vector<double> frameDurations;
    double fps = kDefaultAnimatedMoonFps;
    AnimationLoopMode loopMode = AnimationLoopMode::Forward;
    size_t startFrame = 0;
    bool randomStart = false;
    if (!LoadAnimatedTextureFrameList(folder, frames, fps, frameDurations, loopMode, startFrame, randomStart, true)) {
        return false;
    }
    outPack = parts[0];
    outLabel = parts[1];
    return true;
}

bool HasTextureOptionPath(const std::vector<TextureOption>& options, const std::string& path) {
    for (const TextureOption& option : options) {
        if (StringEqualsIgnoreCase(option.path, path)) {
            return true;
        }
    }
    return false;
}

bool TryTexturePackPath(const std::filesystem::path& root,
                        const std::filesystem::path& path,
                        const TextureSlot& slot,
                        std::string& outPack,
                        std::string& outLabel) {
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    std::vector<std::string> parts;
    if (!ec) {
        for (const std::filesystem::path& part : relative) {
            parts.push_back(part.string());
        }
    }

    if (parts.size() != 3 || !IsTextureFileName(slot, parts[2]) || parts[0].empty() || parts[1].empty()) {
        return false;
    }

    outPack = parts[0];
    outLabel = parts[1];
    return true;
}

void RefreshTextureListLocked(TextureSlot& slot) {
    const std::string oldPath = (slot.selectedOption > 0 && slot.selectedOption <= static_cast<int>(slot.options.size()))
        ? slot.options[static_cast<size_t>(slot.selectedOption - 1)].path
        : std::string{};
    const bool oldAnimated = (slot.selectedOption > 0 && slot.selectedOption <= static_cast<int>(slot.options.size()))
        ? slot.options[static_cast<size_t>(slot.selectedOption - 1)].animated
        : false;

    slot.options.clear();
    const std::filesystem::path root = TextureRootDirectory(slot);
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        Log("[W] %s failed to create texture folder %s error=%d\n", slot.uiLogTag, root.string().c_str(), ec.value());
        ec.clear();
    }
    if (std::filesystem::exists(root, ec)) {
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) {
                continue;
            }

            const std::filesystem::path path = it->path();
            if (StringEqualsIgnoreCase(path.filename().string(), "manifest.json") ||
                StringEqualsIgnoreCase(path.filename().string(), "frame_000.png") ||
                StringEqualsIgnoreCase(path.filename().string(), "frame_000.dds")) {
                std::string pack;
                std::string label;
                const std::filesystem::path folder = path.parent_path();
                if (TryAnimatedTexturePackPath(root, folder, pack, label) && !HasTextureOptionPath(slot.options, folder.string())) {
                    slot.options.push_back({ label + " (" + pack + ")", label, pack, folder.string(), true });
                }
                continue;
            }

            if (!IsTextureFileName(slot, path.filename().string())) {
                continue;
            }

            std::string pack;
            std::string label;
            if (!TryTexturePackPath(root, path, slot, pack, label)) {
                continue;
            }

            slot.options.push_back({ label + " (" + pack + ")", label, pack, path.string(), false });
        }
    }

    std::sort(slot.options.begin(), slot.options.end(), [](const TextureOption& a, const TextureOption& b) {
        const int packCmp = _stricmp(a.pack.c_str(), b.pack.c_str());
        if (packCmp != 0) {
            return packCmp < 0;
        }
        return _stricmp(a.label.c_str(), b.label.c_str()) < 0;
    });
    slot.options.insert(slot.options.begin(), {
        BlankTextureOptionName(slot),
        BlankTextureOptionName(slot),
        "",
        BlankTextureSentinelPath(slot),
        false
    });

    slot.selectedOption = 0;
    if (!oldPath.empty()) {
        for (size_t i = 0; i < slot.options.size(); ++i) {
            if (slot.options[i].animated == oldAnimated && StringEqualsIgnoreCase(slot.options[i].path, oldPath)) {
                slot.selectedOption = static_cast<int>(i + 1);
                break;
            }
        }
    }

    slot.optionsScanned = true;
    Log("[%s] scanned %zu texture option(s) under %s\n", slot.uiLogTag, slot.options.size(), root.string().c_str());
}

void RefreshMoonTextureListLocked() {
    RefreshTextureListLocked(g_moonSlot);
}

void RefreshMilkywayTextureListLocked() {
    RefreshTextureListLocked(g_milkywaySlot);
}

const TextureOption* SelectedTextureLocked(TextureSlot& slot) {
    if (!slot.optionsScanned) {
        RefreshTextureListLocked(slot);
    }

    if (slot.selectedOption <= 0 || slot.selectedOption > static_cast<int>(slot.options.size())) {
        return nullptr;
    }

    return &slot.options[static_cast<size_t>(slot.selectedOption - 1)];
}

const TextureOption* SelectedMoonTextureLocked() {
    return SelectedTextureLocked(g_moonSlot);
}

const TextureOption* SelectedMilkywayTextureLocked() {
    return SelectedTextureLocked(g_milkywaySlot);
}

const char* FormatName(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_BC1_TYPELESS: return "BC1_TYPELESS";
    case DXGI_FORMAT_BC1_UNORM: return "BC1_UNORM";
    case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_UNORM_SRGB";
    case DXGI_FORMAT_BC3_TYPELESS: return "BC3_TYPELESS";
    case DXGI_FORMAT_BC3_UNORM: return "BC3_UNORM";
    case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_UNORM_SRGB";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_BC7_UNORM: return "BC7_UNORM";
    case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_UNORM_SRGB";
    case DXGI_FORMAT_BC6H_UF16: return "BC6H_UF16";
    case DXGI_FORMAT_BC6H_SF16: return "BC6H_SF16";
    default: return "other";
    }
}

constexpr UINT kBc1BlockBytes = 8;

uint64_t Fnv1a64Rows(const uint8_t* data, UINT rowPitch, UINT rowBytes, UINT rows) {
    uint64_t hash = 0xcbf29ce484222325ull;
    for (UINT y = 0; y < rows; ++y) {
        const uint8_t* row = data + static_cast<size_t>(y) * rowPitch;
        for (UINT x = 0; x < rowBytes; ++x) {
            hash ^= row[x];
            hash *= 0x100000001b3ull;
        }
    }
    return hash;
}

int LogSkyExceptionFilter(const char* context, EXCEPTION_POINTERS* info) {
    g_skyTextureHooksDisabled.store(true);
    const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    void* address = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : nullptr;
#if defined(_M_X64)
    const DWORD64 rip = info && info->ContextRecord ? info->ContextRecord->Rip : 0;
    Log("[E] sky texture exception in %s code=0x%08lX address=%p rip=0x%llX; disabling texture switching hooks\n",
        context ? context : "unknown",
        code,
        address,
        static_cast<unsigned long long>(rip));
#else
    Log("[E] sky texture exception in %s code=0x%08lX address=%p; disabling texture switching hooks\n",
        context ? context : "unknown",
        code,
        address);
#endif
    SetTextureStatus(g_moonSlot, "Moon texture: disabled after hook crash");
    SetTextureStatus(g_milkywaySlot, "Milky Way texture: disabled after hook crash");
    return EXCEPTION_EXECUTE_HANDLER;
}

bool IsBc1Format(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_BC1_TYPELESS ||
           format == DXGI_FORMAT_BC1_UNORM ||
           format == DXGI_FORMAT_BC1_UNORM_SRGB;
}

bool RequestsSrgbView(DXGI_FORMAT requested) {
    switch (requested) {
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

DXGI_FORMAT SrvFormatForFileTexture(DXGI_FORMAT fileFormat, DXGI_FORMAT requested) {
    const bool srgb = RequestsSrgbView(requested);
    switch (fileFormat) {
    case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
        return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
        return srgb ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
        return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC7_TYPELESS:
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
        return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return fileFormat != DXGI_FORMAT_UNKNOWN ? fileFormat : requested;
    }
}

bool IsNativeDesc(const TextureSlot& slot, const D3D12_RESOURCE_DESC* desc) {
    return desc &&
           desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc->Width == slot.nativeWidth &&
           desc->Height == slot.nativeHeight &&
           desc->DepthOrArraySize == 1 &&
           desc->MipLevels == slot.nativeMips &&
           IsBc1Format(desc->Format);
}

bool IsNativeDesc1(const TextureSlot& slot, const D3D12_RESOURCE_DESC1* desc) {
    return desc &&
           desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc->Width == slot.nativeWidth &&
           desc->Height == slot.nativeHeight &&
           desc->DepthOrArraySize == 1 &&
           desc->MipLevels == slot.nativeMips &&
           IsBc1Format(desc->Format);
}

uintptr_t ExeImageEnd() {
    auto* module = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    if (!module) {
        return 0;
    }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(module) + nt->OptionalHeader.SizeOfImage;
}

void LogNativeStack(TextureSlot& slot, const char* tag) {
    const uint32_t index = slot.nativeLogCount.fetch_add(1);
    if (index >= 3) {
        if (index == 3) {
            CW_SKY_VERBOSE_LOG("[%s] stack log cap reached\n", slot.logTag);
        }
        return;
    }

    void* frames[40] = {};
    const USHORT count = CaptureStackBackTrace(1, 40, frames, nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    const uintptr_t end = ExeImageEnd();
    char line[512] = {};
    size_t used = 0;
    for (USHORT i = 0; i < count && used + 24 < sizeof(line); ++i) {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(frames[i]);
        if (base && end && addr >= base && addr < end) {
            used += sprintf_s(line + used, sizeof(line) - used, " 0x%llX",
                static_cast<unsigned long long>(addr - base));
        }
    }
    CW_SKY_VERBOSE_LOG("[%s] stack %s:%s\n", slot.logTag, tag, used ? line : " <no game frames>");
}

bool CurrentStackHasLoaderFingerprint(TextureSlot& slot) {
    if (!ResolveFingerprintTargets(slot)) {
        return false;
    }

    void* frames[48] = {};
    const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
    int hits = 0;
    std::vector<bool> matchedTargets(slot.fingerprintTargetCount, false);
    for (USHORT i = 0; i < count; ++i) {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(frames[i]);
        for (size_t targetIndex = 0; targetIndex < slot.fingerprintTargetCount; ++targetIndex) {
            const TextureFingerprintTarget& target = slot.fingerprintTargets[targetIndex];
            if (!target.start || matchedTargets[targetIndex]) {
                continue;
            }
            if (addr >= target.start && addr < target.start + target.range) {
                matchedTargets[targetIndex] = true;
                ++hits;
                break;
            }
        }
    }

    return hits >= slot.fingerprintHitsRequired;
}

bool ShouldTrackNativeDesc(TextureSlot& slot, const D3D12_RESOURCE_DESC* desc) {
    if (!IsNativeDesc(slot, desc) || (slot.lockFirstNative && slot.nativeLocked.load())) {
        return false;
    }
    const bool matched = CurrentStackHasLoaderFingerprint(slot);
    if (!matched && slot.useProof) {
        const uint32_t count = slot.fingerprintRejectCount.fetch_add(1) + 1;
        if (count == 128) {
            Log("[%s] unmatched %ux%u BC1/%umip allocation count=%u; if texture never proves, fingerprint AOB may need a game-patch update\n",
                slot.logTag,
                slot.nativeWidth,
                slot.nativeHeight,
                slot.nativeMips,
                count);
        }
    }
    return matched;
}

bool ShouldTrackNativeDesc1(TextureSlot& slot, const D3D12_RESOURCE_DESC1* desc) {
    if (!IsNativeDesc1(slot, desc) || (slot.lockFirstNative && slot.nativeLocked.load())) {
        return false;
    }
    const bool matched = CurrentStackHasLoaderFingerprint(slot);
    if (!matched && slot.useProof) {
        const uint32_t count = slot.fingerprintRejectCount.fetch_add(1) + 1;
        if (count == 128) {
            Log("[%s] unmatched %ux%u BC1/%umip allocation count=%u; if texture never proves, fingerprint AOB may need a game-patch update\n",
                slot.logTag,
                slot.nativeWidth,
                slot.nativeHeight,
                slot.nativeMips,
                count);
        }
    }
    return matched;
}

const char* MoonProofNameLocked() {
    if (!g_moonSlot.primaryNativeResource) {
        return "waiting-native";
    }
    if (!g_moonSlot.contentProofHit) {
        return "waiting-content";
    }
    if (!g_moonSlot.sawUnormSrv || !g_moonSlot.sawSrgbSrv) {
        return "waiting-srv-pair";
    }
    if (g_moonSlot.copiedSrvCount < 2 || g_moonSlot.bindings.size() < 4) {
        return "waiting-runtime-copy";
    }
    return g_moonSlot.bindings.size() >= 6 ? "proven-strong" : "proven";
}

int MoonProofLevelLocked() {
    if (!g_moonSlot.primaryNativeResource) {
        return 0;
    }
    if (!g_moonSlot.contentProofHit) {
        return 1;
    }
    if (!g_moonSlot.sawUnormSrv || !g_moonSlot.sawSrgbSrv) {
        return 2;
    }
    if (g_moonSlot.copiedSrvCount < 2 || g_moonSlot.bindings.size() < 4) {
        return 3;
    }
    return g_moonSlot.bindings.size() >= 6 ? 5 : 4;
}

void UpdateMoonProofStateLocked(const char* reason) {
    const int level = MoonProofLevelLocked();
    if (level == g_moonSlot.lastProofLevel) {
        return;
    }

    g_moonSlot.lastProofLevel = level;
    SetTextureStatus(g_moonSlot, "Moon texture: %s, bindings=%zu source=%u copied=%u",
        MoonProofNameLocked(),
        g_moonSlot.bindings.size(),
        g_moonSlot.sourceSrvCount,
        g_moonSlot.copiedSrvCount);
    CW_SKY_VERBOSE_LOG("[moon-main] proof %s state=%s level=%d native=%p candidates=%zu bindings=%zu source=%u copied=%u srvPair=%u/%u\n",
        reason,
        MoonProofNameLocked(),
        level,
        g_moonSlot.primaryNativeResource,
        g_moonSlot.nativeResources.size(),
        g_moonSlot.bindings.size(),
        g_moonSlot.sourceSrvCount,
        g_moonSlot.copiedSrvCount,
        g_moonSlot.sawUnormSrv ? 1u : 0u,
        g_moonSlot.sawSrgbSrv ? 1u : 0u);
}

bool MoonSwapReadyLocked() {
    return MoonProofLevelLocked() >= 4;
}

bool NonProofSwapReadyLocked(const TextureSlot& slot) {
    if (slot.requireContentProof && !slot.contentProofHit) {
        return false;
    }
    return !slot.nativeResources.empty() && !slot.bindings.empty();
}

bool TextureSwapReadyLocked(const TextureSlot& slot) {
    return slot.useProof ? MoonSwapReadyLocked() : NonProofSwapReadyLocked(slot);
}

HRESULT CreateCommittedResourceRaw(ID3D12Device* device,
                                   const D3D12_HEAP_PROPERTIES* heapProperties,
                                   D3D12_HEAP_FLAGS heapFlags,
                                   const D3D12_RESOURCE_DESC* desc,
                                   D3D12_RESOURCE_STATES initialState,
                                   REFIID riid,
                                   void** resource) {
    if (g_realCreateCommittedResource) {
        return g_realCreateCommittedResource(device, heapProperties, heapFlags, desc, initialState, nullptr, riid, resource);
    }
    return device->CreateCommittedResource(heapProperties, heapFlags, desc, initialState, nullptr, riid, resource);
}

ID3D12CommandQueue* EnsureUploadQueue(ID3D12Device* device) {
    if (g_hookShuttingDown.load()) {
        return nullptr;
    }
    if (ID3D12CommandQueue* queue = g_uploadQueue.load()) {
        return queue;
    }
    if (!device) {
        return nullptr;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    ID3D12CommandQueue* queue = nullptr;
    HRESULT hr = device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    if (FAILED(hr) || !queue) {
        Log("[moon-main] create upload queue failed hr=0x%08X\n", static_cast<unsigned>(hr));
        return nullptr;
    }

    ID3D12CommandQueue* expected = nullptr;
    if (g_hookShuttingDown.load() || !g_uploadQueue.compare_exchange_strong(expected, queue)) {
        queue->Release();
    }
    return g_uploadQueue.load();
}

CachedTexture* FindCachedTextureLocked(TextureSlot& slot, const std::string& path) {
    if (path.empty()) {
        return nullptr;
    }
    for (CachedTexture& cached : slot.cache) {
        if (StringEqualsIgnoreCase(cached.path, path)) {
            return &cached;
        }
    }
    return nullptr;
}

void ActivateCachedTextureLocked(TextureSlot& slot, const CachedTexture& cached) {
    slot.activePath = cached.path;
    slot.activeAnimated = false;
    slot.fileFormat = cached.format;
    slot.fileMips = cached.mips;
    slot.fileWidth = cached.width;
    slot.fileHeight = cached.height;
    slot.fileResource.store(cached.resource);
}

void DeactivateFileResourceLocked(TextureSlot& slot) {
    size_t queuedAnimationRequests = 0;
    if (slot.activeAnimated && !slot.activePath.empty()) {
        queuedAnimationRequests = RemoveAnimationLoadRequestsLocked(slot, slot.activePath.c_str());
        for (CachedAnimation& cached : slot.animCache) {
            if (!StringEqualsIgnoreCase(cached.path, slot.activePath)) {
                continue;
            }

            size_t residentSlots = 0;
            UINT64 residentBytes = 0;
            for (const AnimatedFrameSlot& frameSlot : cached.frameSlots) {
                if (frameSlot.resource) {
                    ++residentSlots;
                    residentBytes += frameSlot.gpuEstimateBytes;
                }
            }
            const int restoredDescriptors = TextureSwapReadyLocked(slot) ? RewriteTrackedDescriptorsLocked(slot, nullptr) : 0;
            slot.fileResource.store(nullptr);
            ReleaseAnimationResources(cached);
            slot.animResidentGpuEstimateBytes = 0;
            slot.animPeakResidentGpuEstimateBytes = 0;
            Log("[%s] animation deactivated path=%s gpuSlots=%zu restoredDescriptors=%d releasedSlots=%zu releasedGpuApprox=%.2f MiB queuedDrops=%zu metadata=kept cachedAnimations=%zu\n",
                slot.logTag,
                cached.path.c_str(),
                AnimationGpuSlotCapacity(cached),
                restoredDescriptors,
                residentSlots,
                BytesToMiB(residentBytes),
                queuedAnimationRequests,
                slot.animCache.size());
            break;
        }
    } else {
        queuedAnimationRequests = RemoveAnimationLoadRequestsLocked(slot, nullptr);
    }

    slot.activePath.clear();
    slot.activeAnimated = false;
    slot.fileResource.store(nullptr);
    slot.fileFormat = DXGI_FORMAT_UNKNOWN;
    slot.fileMips = 0;
    slot.fileWidth = 0;
    slot.fileHeight = 0;
    slot.animFrameIndex = 0;
    slot.animStartFrame = 0;
    slot.animDirection = 1;
    slot.animStartDirection = 1;
    slot.animStartTick = 0;
    slot.animLastTick = 0;
    slot.animFrameApplied = false;
    slot.animQueuedWindowValid = false;
    slot.animQueuedWindowFrame = static_cast<size_t>(-1);
    slot.animQueuedWindowDirection = 1;
}

void EnforceCacheLimitLocked(TextureSlot& slot) {
    constexpr size_t kTextureCacheLimit = 4;
    while (slot.cache.size() > kTextureCacheLimit) {
        auto evict = slot.cache.end();
        for (auto it = slot.cache.begin(); it != slot.cache.end(); ++it) {
            if (!StringEqualsIgnoreCase(it->path, slot.activePath)) {
                evict = it;
                break;
            }
        }
        if (evict == slot.cache.end()) {
            return;
        }

        if (evict->resource) {
            evict->resource->Release();
        }
        CW_SKY_VERBOSE_LOG("[%s] cache evicted path=%s cached=%zu\n", slot.logTag, evict->path.c_str(), slot.cache.size() - 1);
        slot.cache.erase(evict);
    }
}

void ReleaseAnimationResources(CachedAnimation& cached) {
    for (AnimatedFrameSlot& slot : cached.frameSlots) {
        if (slot.resource) {
            slot.resource->Release();
        }
        slot = {};
    }
}

double BytesToMiB(UINT64 bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

UINT64 AnimationResidentGpuBytes(const CachedAnimation& cached) {
    UINT64 bytes = 0;
    for (const AnimatedFrameSlot& slot : cached.frameSlots) {
        if (slot.resource) {
            bytes += slot.gpuEstimateBytes;
        }
    }
    return bytes;
}

size_t AnimationResidentSlotCount(const CachedAnimation& cached) {
    size_t count = 0;
    for (const AnimatedFrameSlot& slot : cached.frameSlots) {
        if (slot.resource) {
            ++count;
        }
    }
    return count;
}

UINT64 ImageSourceBytes(const SkyImageData& image) {
    UINT64 bytes = 0;
    for (const SkyImageData::MipLevel& mip : image.mipLevels) {
        bytes += static_cast<UINT64>(mip.byteSize);
    }
    return bytes;
}

void UpdateAnimationResidentStatsLocked(TextureSlot& owner, const CachedAnimation& cached) {
    owner.animResidentGpuEstimateBytes = AnimationResidentGpuBytes(cached);
    owner.animPeakResidentGpuEstimateBytes = std::max(owner.animPeakResidentGpuEstimateBytes, owner.animResidentGpuEstimateBytes);
}

void ResetAnimationDiagnosticsLocked(TextureSlot& slot) {
    slot.animFrameLoadCount = 0;
    slot.animFrameMissCount = 0;
    slot.animFrameEvictCount = 0;
    slot.animRewriteCount = 0;
    slot.animFrameQueueCount = 0;
    slot.animFrameSkipCount = 0;
    slot.animFrameStaleDropCount = 0;
    slot.animResidentGpuEstimateBytes = 0;
    slot.animPeakResidentGpuEstimateBytes = 0;
    slot.animLastLoadMs = 0;
    slot.animPeakLoadMs = 0;
    slot.animLastDiagnosticTick = 0;
    slot.animDiagnosticLogCount = 0;
}

void LogAnimationDiagnosticsLocked(TextureSlot& slot, const CachedAnimation& cached, const char* reason, bool force = false) {
    const ULONGLONG now = GetTickCount64();
    constexpr ULONGLONG kDiagnosticIntervalMs = 5000;
    if (!force && slot.animLastDiagnosticTick != 0 && now - slot.animLastDiagnosticTick < kDiagnosticIntervalMs) {
        return;
    }

    UpdateAnimationResidentStatsLocked(slot, cached);
    slot.animLastDiagnosticTick = now;
    ++slot.animDiagnosticLogCount;

    Log("[%s] animation stats reason=%s frame=%zu/%zu slots=%zu/%zu gpuSlots=%zu gpuApprox=%.2f MiB peak=%.2f MiB loads=%llu queued=%llu misses=%llu skipped=%llu evictions=%llu staleDrops=%llu rewrites=%llu lastLoadMs=%lu peakLoadMs=%lu descriptors=%zu sample=%u\n",
        slot.logTag,
        reason && reason[0] ? reason : "periodic",
        slot.animFrameIndex,
        cached.framePaths.size(),
        AnimationResidentSlotCount(cached),
        AnimationGpuSlotCapacity(cached),
        AnimationGpuSlotCapacity(cached),
        BytesToMiB(slot.animResidentGpuEstimateBytes),
        BytesToMiB(slot.animPeakResidentGpuEstimateBytes),
        static_cast<unsigned long long>(slot.animFrameLoadCount),
        static_cast<unsigned long long>(slot.animFrameQueueCount),
        static_cast<unsigned long long>(slot.animFrameMissCount),
        static_cast<unsigned long long>(slot.animFrameSkipCount),
        static_cast<unsigned long long>(slot.animFrameEvictCount),
        static_cast<unsigned long long>(slot.animFrameStaleDropCount),
        static_cast<unsigned long long>(slot.animRewriteCount),
        static_cast<unsigned long>(slot.animLastLoadMs),
        static_cast<unsigned long>(slot.animPeakLoadMs),
        slot.bindings.size(),
        slot.animDiagnosticLogCount);
}

size_t AnimationFrameCount(const CachedAnimation& cached) {
    return cached.framePaths.size();
}

size_t AnimationStepPreviewIndexWithDirection(const CachedAnimation& cached, size_t frameIndex, int& direction, size_t steps);
bool AnimationFrameInPlaybackWindow(const CachedAnimation& cached, size_t windowFrame, int windowDirection, size_t candidateFrame);

ID3D12Resource* FindAnimationFrameResourceLocked(CachedAnimation& cached, size_t frameIndex, bool markUsed = true) {
    for (AnimatedFrameSlot& slot : cached.frameSlots) {
        if (slot.resource && slot.frameIndex == frameIndex) {
            if (markUsed) {
                slot.lastUsedTick = GetTickCount64();
            }
            return slot.resource;
        }
    }
    return nullptr;
}

AnimatedFrameSlot* SelectAnimationFrameSlotLocked(TextureSlot& owner, CachedAnimation& cached, size_t frameIndex) {
    for (AnimatedFrameSlot& slot : cached.frameSlots) {
        if (slot.resource && slot.frameIndex == frameIndex) {
            return &slot;
        }
    }

    for (AnimatedFrameSlot& slot : cached.frameSlots) {
        if (!slot.resource) {
            return &slot;
        }
    }

    AnimatedFrameSlot* evict = nullptr;
    ID3D12Resource* currentPublished = owner.fileResource.load();
    for (AnimatedFrameSlot& slot : cached.frameSlots) {
        if (slot.frameIndex == owner.animFrameIndex ||
            AnimationFrameInPlaybackWindow(cached, owner.animFrameIndex, owner.animDirection, slot.frameIndex) ||
            (currentPublished && slot.resource == currentPublished)) {
            continue;
        }
        if (!evict || slot.lastUsedTick < evict->lastUsedTick) {
            evict = &slot;
        }
    }

    return evict;
}

ID3D12Resource* StoreAnimationFrameResourceLocked(TextureSlot& owner,
                                                  CachedAnimation& cached,
                                                  size_t frameIndex,
                                                  ID3D12Resource* resource,
                                                  UINT64 gpuEstimateBytes,
                                                  UINT64 sourceBytes,
                                                  DWORD loadMs) {
    if (!resource || frameIndex >= AnimationFrameCount(cached)) {
        return nullptr;
    }

    if (ID3D12Resource* existing = FindAnimationFrameResourceLocked(cached, frameIndex)) {
        resource->Release();
        UpdateAnimationResidentStatsLocked(owner, cached);
        return existing;
    }

    AnimatedFrameSlot* slot = SelectAnimationFrameSlotLocked(owner, cached, frameIndex);
    if (!slot) {
        CW_SKY_VERBOSE_LOG("[%s] animation frame load skipped: no evictable slot frame=%zu active=%zu published=%p\n",
            owner.logTag,
            frameIndex,
            owner.animFrameIndex,
            owner.fileResource.load());
        resource->Release();
        return nullptr;
    }
    if (slot->resource) {
        ++owner.animFrameEvictCount;
        CW_SKY_VERBOSE_LOG("[%s] animation frame evicted index=%zu bytes=%.2f MiB\n",
            owner.logTag,
            slot->frameIndex,
            BytesToMiB(slot->gpuEstimateBytes));
        slot->resource->Release();
    }
    slot->frameIndex = frameIndex;
    slot->resource = resource;
    slot->lastUsedTick = GetTickCount64();
    slot->gpuEstimateBytes = gpuEstimateBytes;
    slot->sourceBytes = sourceBytes;
    slot->loadMs = loadMs;
    ++owner.animFrameLoadCount;
    owner.animLastLoadMs = loadMs;
    owner.animPeakLoadMs = std::max(owner.animPeakLoadMs, loadMs);
    UpdateAnimationResidentStatsLocked(owner, cached);
    CW_SKY_VERBOSE_LOG("[%s] animation frame resident index=%zu slots=%zu/%zu gpuApprox=%.2f MiB peak=%.2f MiB source=%.2f MiB loadMs=%lu\n",
        owner.logTag,
        frameIndex,
        AnimationResidentSlotCount(cached),
        AnimationGpuSlotCapacity(cached),
        BytesToMiB(owner.animResidentGpuEstimateBytes),
        BytesToMiB(owner.animPeakResidentGpuEstimateBytes),
        BytesToMiB(sourceBytes),
        static_cast<unsigned long>(loadMs));
    return resource;
}

CachedAnimation* FindCachedAnimationLocked(TextureSlot& slot, const std::string& path) {
    if (path.empty()) {
        return nullptr;
    }
    for (CachedAnimation& cached : slot.animCache) {
        if (StringEqualsIgnoreCase(cached.path, path)) {
            return &cached;
        }
    }
    return nullptr;
}

CachedAnimation* FindCachedAnimationLocked(TextureSlot& slot, const char* path) {
    if (!path || !path[0]) {
        return nullptr;
    }
    for (CachedAnimation& cached : slot.animCache) {
        if (StringEqualsIgnoreCase(cached.path, path)) {
            return &cached;
        }
    }
    return nullptr;
}

void ActivateCachedAnimationLocked(TextureSlot& slot, CachedAnimation& cached) {
    EnsureAnimationSlotStorage(cached);
    slot.activePath = cached.path;
    slot.activeAnimated = true;
    slot.fileFormat = cached.format;
    slot.fileMips = cached.mips;
    slot.fileWidth = cached.width;
    slot.fileHeight = cached.height;
    size_t startFrame = cached.startFrame;
    const size_t frameCount = AnimationFrameCount(cached);
    if (cached.randomStart && frameCount > 0) {
        LARGE_INTEGER qpc{};
        QueryPerformanceCounter(&qpc);
        const ULONGLONG seed = GetTickCount64() ^
            static_cast<ULONGLONG>(qpc.QuadPart) ^
            static_cast<ULONGLONG>(GetCurrentThreadId());
        startFrame = static_cast<size_t>(seed % frameCount);
    }
    if (startFrame >= frameCount) {
        startFrame = 0;
    }

    slot.animFrameIndex = startFrame;
    slot.animStartFrame = startFrame;
    slot.animDirection = 1;
    if (cached.loopMode == AnimationLoopMode::PingPong && frameCount > 0 && slot.animFrameIndex + 1 >= frameCount) {
        slot.animDirection = -1;
    }
    slot.animStartDirection = slot.animDirection;
    slot.animStartTick = GetTickCount64();
    slot.animLastTick = slot.animStartTick;
    slot.animFrameApplied = false;
    slot.fileResource.store(nullptr);
    slot.animQueuedWindowValid = false;
    slot.animQueuedWindowFrame = static_cast<size_t>(-1);
    slot.animQueuedWindowDirection = slot.animDirection;
    RemoveAnimationLoadRequestsLocked(slot, nullptr);
    ResetAnimationDiagnosticsLocked(slot);
}

void EnforceAnimationCacheLimitLocked(TextureSlot& slot) {
    constexpr size_t kAnimationCacheLimit = 2;
    while (slot.animCache.size() > kAnimationCacheLimit) {
        auto evict = slot.animCache.end();
        for (auto it = slot.animCache.begin(); it != slot.animCache.end(); ++it) {
            if (!StringEqualsIgnoreCase(it->path, slot.activePath)) {
                evict = it;
                break;
            }
        }
        if (evict == slot.animCache.end()) {
            return;
        }

        ReleaseAnimationResources(*evict);
        CW_SKY_VERBOSE_LOG("[%s] animation cache evicted path=%s cached=%zu\n", slot.logTag, evict->path.c_str(), slot.animCache.size() - 1);
        slot.animCache.erase(evict);
    }
}

double AnimationFrameDurationMs(const CachedAnimation& cached, size_t frameIndex) {
    if (!cached.frameDurationsMs.empty() && frameIndex < cached.frameDurationsMs.size()) {
        return std::max(1.0, cached.frameDurationsMs[frameIndex]);
    }
    return std::max(1.0, cached.frameMs);
}

ID3D12Resource* CreateTextureResourceFromImage(TextureSlot& slot,
                                               ID3D12Device* device,
                                               const SkyImageData& image,
                                               const std::string& path,
                                               UINT& outMipCount,
                                               UINT64& outUploadSize) {
    outMipCount = static_cast<UINT>(image.mipLevels.size());
    outUploadSize = 0;
    if (outMipCount == 0 || outMipCount > D3D12_REQ_MIP_LEVELS) {
        SetTextureStatus(slot, "%s: unsupported mip count", slot.displayName);
        Log("[%s] unsupported mip count mips=%u path=%s\n", slot.logTag, outMipCount, path.c_str());
        return nullptr;
    }

    for (UINT mipIndex = 0; mipIndex < outMipCount; ++mipIndex) {
        const SkyImageData::MipLevel& mip = image.mipLevels[mipIndex];
        if (mip.byteOffset > image.pixels.size() || mip.byteSize > image.pixels.size() - mip.byteOffset) {
            SetTextureStatus(slot, "%s: pixel data too small", slot.displayName);
            Log("[%s] mip pixel data too small mip=%u offset=%zu size=%zu total=%zu path=%s\n",
                slot.logTag,
                mipIndex,
                mip.byteOffset,
                mip.byteSize,
                image.pixels.size(),
                path.c_str());
            return nullptr;
        }
    }

    const UINT64 requiredBytes = image.mipLevels[0].sourceRowPitch * image.mipLevels[0].sourceRows;
    if (image.pixels.size() < requiredBytes) {
        SetTextureStatus(slot, "%s: pixel data too small", slot.displayName);
        Log("[%s] pixel data too small have=%zu need=%llu path=%s\n",
            slot.logTag,
            image.pixels.size(),
            static_cast<unsigned long long>(requiredBytes),
            path.c_str());
        return nullptr;
    }

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = image.width;
    texDesc.Height = image.height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = static_cast<UINT16>(outMipCount);
    texDesc.Format = image.format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES defaultHeap{};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_HEAP_PROPERTIES uploadHeap{};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    ID3D12Resource* texture = nullptr;
    HRESULT hr = CreateCommittedResourceRaw(device, &defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&texture));
    if (FAILED(hr) || !texture) {
        SetTextureStatus(slot, "%s: create texture failed", slot.displayName);
        Log("[%s] create selected texture failed hr=0x%08X path=%s\n", slot.logTag, static_cast<unsigned>(hr), path.c_str());
        return nullptr;
    }

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(outMipCount);
    std::vector<UINT> numRows(outMipCount);
    std::vector<UINT64> rowSizes(outMipCount);
    device->GetCopyableFootprints(&texDesc, 0, outMipCount, 0, layouts.data(), numRows.data(), rowSizes.data(), &outUploadSize);

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = outUploadSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ID3D12Resource* upload = nullptr;
    hr = CreateCommittedResourceRaw(device, &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&upload));
    if (FAILED(hr) || !upload) {
        SetTextureStatus(slot, "%s: create upload failed", slot.displayName);
        Log("[%s] create upload buffer failed hr=0x%08X path=%s\n", slot.logTag, static_cast<unsigned>(hr), path.c_str());
        texture->Release();
        return nullptr;
    }

    uint8_t* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    hr = upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr) || !mapped) {
        SetTextureStatus(slot, "%s: upload map failed", slot.displayName);
        Log("[%s] upload map failed hr=0x%08X path=%s\n", slot.logTag, static_cast<unsigned>(hr), path.c_str());
        upload->Release();
        texture->Release();
        return nullptr;
    }

    for (UINT mipIndex = 0; mipIndex < outMipCount; ++mipIndex) {
        const SkyImageData::MipLevel& mip = image.mipLevels[mipIndex];
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout = layouts[mipIndex];
        if (layout.Footprint.RowPitch < mip.sourceRowPitch) {
            SetTextureStatus(slot, "%s: upload row too small", slot.displayName);
            Log("[%s] upload row too small mip=%u layout=%u source=%llu path=%s\n",
                slot.logTag,
                mipIndex,
                layout.Footprint.RowPitch,
                static_cast<unsigned long long>(mip.sourceRowPitch),
                path.c_str());
            upload->Unmap(0, nullptr);
            upload->Release();
            texture->Release();
            return nullptr;
        }

        uint8_t* dst = mapped + layout.Offset;
        const uint8_t* src = image.pixels.data() + mip.byteOffset;
        for (UINT y = 0; y < mip.sourceRows; ++y) {
            memcpy(dst + static_cast<size_t>(y) * layout.Footprint.RowPitch, src + static_cast<size_t>(y) * mip.sourceRowPitch, static_cast<size_t>(mip.sourceRowPitch));
        }
    }
    upload->Unmap(0, nullptr);

    ID3D12CommandQueue* queue = EnsureUploadQueue(device);
    if (!queue) {
        SetTextureStatus(slot, "%s: upload queue failed", slot.displayName);
        upload->Release();
        texture->Release();
        return nullptr;
    }

    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE eventHandle = nullptr;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&allocator));
    if (SUCCEEDED(hr)) {
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list));
    }
    if (FAILED(hr) || !allocator || !list) {
        SetTextureStatus(slot, "%s: command list failed", slot.displayName);
        Log("[%s] command list setup failed hr=0x%08X path=%s\n", slot.logTag, static_cast<unsigned>(hr), path.c_str());
        if (list) list->Release();
        if (allocator) allocator->Release();
        upload->Release();
        texture->Release();
        return nullptr;
    }

    for (UINT mipIndex = 0; mipIndex < outMipCount; ++mipIndex) {
        D3D12_TEXTURE_COPY_LOCATION dstLoc{};
        dstLoc.pResource = texture;
        dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dstLoc.SubresourceIndex = mipIndex;
        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = upload;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        srcLoc.PlacedFootprint = layouts[mipIndex];
        list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(1, &barrier);
    hr = list->Close();
    if (FAILED(hr)) {
        SetTextureStatus(slot, "%s: command close failed", slot.displayName);
        Log("[%s] command list close failed hr=0x%08X path=%s\n", slot.logTag, static_cast<unsigned>(hr), path.c_str());
        list->Release();
        allocator->Release();
        upload->Release();
        texture->Release();
        return nullptr;
    }

    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    if (SUCCEEDED(hr) && fence) {
        eventHandle = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        hr = queue->Signal(fence, 1);
        if (SUCCEEDED(hr) && fence->GetCompletedValue() < 1 && eventHandle) {
            fence->SetEventOnCompletion(1, eventHandle);
            WaitForSingleObject(eventHandle, 5000);
        }
    } else {
        Log("[%s] fence create failed hr=0x%08X path=%s\n", slot.logTag, static_cast<unsigned>(hr), path.c_str());
    }

    if (eventHandle) CloseHandle(eventHandle);
    if (fence) fence->Release();
    list->Release();
    allocator->Release();
    upload->Release();
    return texture;
}

ID3D12Resource* CreateAnimationFrameResource(TextureSlot& slot,
                                             ID3D12Device* device,
                                             const CachedAnimation& animation,
                                             size_t frameIndex,
                                             UINT64& outUploadSize,
                                             UINT64& outSourceBytes,
                                             DWORD& outLoadMs) {
    outUploadSize = 0;
    outSourceBytes = 0;
    outLoadMs = 0;
    if (!device || frameIndex >= animation.framePaths.size()) {
        return nullptr;
    }

    const ULONGLONG startTick = GetTickCount64();
    SkyImageData image{};
    char loadStatus[128] = {};
    const std::string& framePath = animation.framePaths[frameIndex];
    if (!LoadSkyImageFile(framePath, image, loadStatus, sizeof(loadStatus))) {
        outLoadMs = static_cast<DWORD>(std::min<ULONGLONG>(GetTickCount64() - startTick, MAXDWORD));
        SetTextureStatus(slot, "%s: frame load failed", slot.displayName);
        Log("[%s] animation frame load failed index=%zu path=%s status=%s\n",
            slot.logTag,
            frameIndex,
            framePath.c_str(),
            loadStatus);
        return nullptr;
    }

    outSourceBytes = ImageSourceBytes(image);
    const UINT mipCount = static_cast<UINT>(image.mipLevels.size());
    if (animation.format != image.format ||
        animation.mips != mipCount ||
        animation.width != image.width ||
        animation.height != image.height) {
        outLoadMs = static_cast<DWORD>(std::min<ULONGLONG>(GetTickCount64() - startTick, MAXDWORD));
        SetTextureStatus(slot, "%s: mixed animation frame format", slot.displayName);
        Log("[%s] animation frame mismatch index=%zu path=%s got=%ux%u mips=%u fmt=%s(%u) expected=%ux%u mips=%u fmt=%s(%u)\n",
            slot.logTag,
            frameIndex,
            framePath.c_str(),
            image.width,
            image.height,
            mipCount,
            FormatName(image.format),
            static_cast<unsigned>(image.format),
            animation.width,
            animation.height,
            animation.mips,
            FormatName(animation.format),
            static_cast<unsigned>(animation.format));
        return nullptr;
    }

    UINT uploadedMips = 0;
    ID3D12Resource* frame = CreateTextureResourceFromImage(slot, device, image, framePath, uploadedMips, outUploadSize);
    if (frame && uploadedMips != animation.mips) {
        Log("[%s] animation frame uploaded mip mismatch index=%zu got=%u expected=%u\n",
            slot.logTag,
            frameIndex,
            uploadedMips,
            animation.mips);
    }
    outLoadMs = static_cast<DWORD>(std::min<ULONGLONG>(GetTickCount64() - startTick, MAXDWORD));
    return frame;
}

bool EnsureAnimationFrameLoaded(TextureSlot& slot,
                                const char* activePath,
                                size_t frameIndex,
                                ID3D12Device* device,
                                ID3D12Resource** outResource = nullptr) {
    if (outResource) {
        *outResource = nullptr;
    }
    if (!device || !activePath || !activePath[0]) {
        return false;
    }

    EnterCriticalSection(&g_hookLock);
    CachedAnimation* cached = FindCachedAnimationLocked(slot, activePath);
    if (!cached || frameIndex >= AnimationFrameCount(*cached)) {
        LeaveCriticalSection(&g_hookLock);
        return false;
    }
    if (!AnimationFrameInPlaybackWindow(*cached, slot.animFrameIndex, slot.animDirection, frameIndex)) {
        ++slot.animFrameStaleDropCount;
        LeaveCriticalSection(&g_hookLock);
        return false;
    }
    if (ID3D12Resource* existing = FindAnimationFrameResourceLocked(*cached, frameIndex)) {
        if (outResource) {
            *outResource = existing;
        }
        LeaveCriticalSection(&g_hookLock);
        return true;
    }
    CachedAnimation metadata = *cached;
    LeaveCriticalSection(&g_hookLock);

    UINT64 uploadSize = 0;
    UINT64 sourceBytes = 0;
    DWORD loadMs = 0;
    ID3D12Resource* loaded = CreateAnimationFrameResource(slot, device, metadata, frameIndex, uploadSize, sourceBytes, loadMs);
    if (!loaded) {
        return false;
    }

    EnterCriticalSection(&g_hookLock);
    cached = FindCachedAnimationLocked(slot, activePath);
    if (!cached || !slot.activeAnimated || !StringEqualsIgnoreCase(slot.activePath, activePath) ||
        frameIndex >= AnimationFrameCount(*cached) ||
        !AnimationFrameInPlaybackWindow(*cached, slot.animFrameIndex, slot.animDirection, frameIndex)) {
        if (slot.activeAnimated && StringEqualsIgnoreCase(slot.activePath, activePath)) {
            ++slot.animFrameStaleDropCount;
        }
        LeaveCriticalSection(&g_hookLock);
        loaded->Release();
        return false;
    }
    ID3D12Resource* stored = StoreAnimationFrameResourceLocked(slot, *cached, frameIndex, loaded, uploadSize, sourceBytes, loadMs);
    if (stored && outResource) {
        *outResource = stored;
    }
    LeaveCriticalSection(&g_hookLock);
    return stored != nullptr;
}

size_t AnimationStepPreviewIndex(const CachedAnimation& cached, size_t frameIndex, int direction, size_t steps) {
    const size_t frameCount = AnimationFrameCount(cached);
    if (frameCount == 0) {
        return 0;
    }
    if (frameIndex >= frameCount) {
        frameIndex = 0;
    }
    if (steps == 0 || cached.loopMode == AnimationLoopMode::Hold) {
        return frameIndex;
    }
    if (cached.loopMode == AnimationLoopMode::Once) {
        return std::min(frameCount - 1, frameIndex + steps);
    }
    if (cached.loopMode != AnimationLoopMode::PingPong || frameCount < 3) {
        return (frameIndex + steps) % frameCount;
    }

    int previewDirection = direction >= 0 ? 1 : -1;
    size_t previewFrame = frameIndex;
    for (size_t i = 0; i < steps; ++i) {
        if (previewDirection >= 0) {
            if (previewFrame + 1 >= frameCount) {
                previewDirection = -1;
                --previewFrame;
            } else {
                ++previewFrame;
            }
        } else {
            if (previewFrame == 0) {
                previewDirection = 1;
                ++previewFrame;
            } else {
                --previewFrame;
            }
        }
    }
    return previewFrame;
}

size_t AnimationStepPreviewIndexWithDirection(const CachedAnimation& cached, size_t frameIndex, int& direction, size_t steps) {
    const size_t frameCount = AnimationFrameCount(cached);
    if (frameCount == 0) {
        direction = 1;
        return 0;
    }
    if (frameIndex >= frameCount) {
        frameIndex = 0;
        direction = 1;
    }
    if (steps == 0 || cached.loopMode == AnimationLoopMode::Hold) {
        return frameIndex;
    }
    if (cached.loopMode == AnimationLoopMode::Once) {
        return std::min(frameCount - 1, frameIndex + steps);
    }
    if (cached.loopMode != AnimationLoopMode::PingPong || frameCount < 3) {
        return (frameIndex + steps) % frameCount;
    }

    size_t previewFrame = frameIndex;
    for (size_t i = 0; i < steps; ++i) {
        if (direction >= 0) {
            if (previewFrame + 1 >= frameCount) {
                direction = -1;
                --previewFrame;
            } else {
                ++previewFrame;
            }
        } else {
            if (previewFrame == 0) {
                direction = 1;
                ++previewFrame;
            } else {
                --previewFrame;
            }
        }
    }
    return previewFrame;
}

bool AnimationFrameInPlaybackWindow(const CachedAnimation& cached, size_t windowFrame, int windowDirection, size_t candidateFrame) {
    const size_t frameCount = AnimationFrameCount(cached);
    if (frameCount == 0 || candidateFrame >= frameCount) {
        return false;
    }
    int direction = windowDirection >= 0 ? 1 : -1;
    size_t frame = windowFrame < frameCount ? windowFrame : 0;
    const size_t windowSteps = std::min(AnimationGpuSlotCapacity(cached), frameCount);
    for (size_t step = 0; step < windowSteps; ++step) {
        if (frame == candidateFrame) {
            return true;
        }
        frame = AnimationStepPreviewIndexWithDirection(cached, frame, direction, 1);
    }
    return false;
}

double AnimationPlaybackPeriodMs(const CachedAnimation& cached, size_t startFrame, int startDirection) {
    const size_t frameCount = AnimationFrameCount(cached);
    if (frameCount == 0 || cached.loopMode == AnimationLoopMode::Hold || cached.loopMode == AnimationLoopMode::Once) {
        return 0.0;
    }

    const size_t periodSteps = (cached.loopMode == AnimationLoopMode::PingPong && frameCount >= 3)
        ? (frameCount * 2u - 2u)
        : frameCount;
    size_t frame = std::min(startFrame, frameCount - 1);
    int direction = startDirection >= 0 ? 1 : -1;
    double period = 0.0;
    for (size_t i = 0; i < periodSteps; ++i) {
        period += AnimationFrameDurationMs(cached, frame);
        frame = AnimationStepPreviewIndexWithDirection(cached, frame, direction, 1);
    }
    return period;
}

size_t AnimationFrameAtElapsedMs(const CachedAnimation& cached,
                                 size_t startFrame,
                                 int startDirection,
                                 ULONGLONG elapsedMs,
                                 int& outDirection) {
    const size_t frameCount = AnimationFrameCount(cached);
    outDirection = startDirection >= 0 ? 1 : -1;
    if (frameCount == 0) {
        return 0;
    }

    size_t frame = std::min(startFrame, frameCount - 1);
    if (cached.loopMode == AnimationLoopMode::Hold) {
        return frame;
    }

    double remainingMs = static_cast<double>(elapsedMs);
    const double periodMs = AnimationPlaybackPeriodMs(cached, frame, outDirection);
    if (periodMs > 0.0) {
        remainingMs = fmod(remainingMs, periodMs);
    }

    const size_t maxSteps = cached.loopMode == AnimationLoopMode::Once
        ? frameCount
        : std::max<size_t>(frameCount * 2u, 1u);
    for (size_t step = 0; step < maxSteps; ++step) {
        const double frameMs = AnimationFrameDurationMs(cached, frame);
        if (remainingMs < frameMs) {
            break;
        }
        remainingMs -= frameMs;
        const size_t nextFrame = AnimationStepPreviewIndexWithDirection(cached, frame, outDirection, 1);
        if (nextFrame == frame && cached.loopMode == AnimationLoopMode::Once) {
            break;
        }
        frame = nextFrame;
    }
    return frame;
}

void QueueAnimationFrameLoadLocked(TextureSlot& slot,
                                   const char* activePath,
                                   size_t frameIndex,
                                   uint32_t priority) {
    if (!activePath || !activePath[0]) {
        return;
    }
    CachedAnimation* cached = FindCachedAnimationLocked(slot, activePath);
    if (!cached || frameIndex >= AnimationFrameCount(*cached)) {
        return;
    }
    if (FindAnimationFrameResourceLocked(*cached, frameIndex, false)) {
        return;
    }
    for (const AnimationLoadRequest& request : g_animationLoadRequests) {
        if (request.slot == &slot &&
            request.frameIndex == frameIndex &&
            StringEqualsIgnoreCase(request.path, activePath)) {
            return;
        }
    }
    g_animationLoadRequests.push_back({ &slot, activePath, frameIndex, priority, GetTickCount64() });
    ++slot.animFrameQueueCount;
}

void QueueAnimationWindowLocked(TextureSlot& slot,
                                CachedAnimation& cached,
                                size_t frameIndex,
                                int direction,
                                bool includeCurrent) {
    if (slot.activePath.empty()) {
        return;
    }
    if (slot.animQueuedWindowValid &&
        slot.animQueuedWindowFrame == frameIndex &&
        slot.animQueuedWindowDirection == direction) {
        return;
    }

    const std::string activePath = slot.activePath;
    const size_t dropped = RemoveAnimationLoadRequestsLocked(slot, activePath.c_str());
    slot.animFrameStaleDropCount += static_cast<uint64_t>(dropped);

    if (includeCurrent) {
        QueueAnimationFrameLoadLocked(slot, activePath.c_str(), frameIndex, 0);
    }
    const size_t windowSteps = std::min(AnimationGpuSlotCapacity(cached), AnimationFrameCount(cached));
    for (size_t step = 1; step < windowSteps; ++step) {
        const size_t preloadIndex = AnimationStepPreviewIndex(cached, frameIndex, direction, step);
        QueueAnimationFrameLoadLocked(slot, activePath.c_str(), preloadIndex, static_cast<uint32_t>(step));
    }

    slot.animQueuedWindowValid = true;
    slot.animQueuedWindowFrame = frameIndex;
    slot.animQueuedWindowDirection = direction;
    if (g_animationWorkerEvent) {
        SetEvent(g_animationWorkerEvent);
    }
}

bool AnimationLoadRequestCurrentLocked(TextureSlot& slot, const AnimationLoadRequest& request) {
    if (request.slot != &slot || !slot.activeAnimated || !StringEqualsIgnoreCase(slot.activePath, request.path)) {
        return false;
    }
    CachedAnimation* cached = FindCachedAnimationLocked(slot, request.path);
    if (!cached || request.frameIndex >= AnimationFrameCount(*cached)) {
        return false;
    }
    if (FindAnimationFrameResourceLocked(*cached, request.frameIndex, false)) {
        return false;
    }
    return AnimationFrameInPlaybackWindow(*cached, slot.animFrameIndex, slot.animDirection, request.frameIndex);
}

bool PopAnimationLoadRequest(AnimationLoadRequest& outRequest) {
    EnterCriticalSection(&g_hookLock);
    while (!g_animationLoadRequests.empty()) {
        auto best = g_animationLoadRequests.begin();
        for (auto it = g_animationLoadRequests.begin() + 1; it != g_animationLoadRequests.end(); ++it) {
            if (it->priority < best->priority ||
                (it->priority == best->priority && it->queuedTick < best->queuedTick)) {
                best = it;
            }
        }

        TextureSlot* slot = best->slot;
        if (slot && AnimationLoadRequestCurrentLocked(*slot, *best)) {
            outRequest = *best;
            g_animationLoadRequests.erase(best);
            LeaveCriticalSection(&g_hookLock);
            return true;
        }

        if (slot) {
            ++slot->animFrameStaleDropCount;
        }
        g_animationLoadRequests.erase(best);
    }
    LeaveCriticalSection(&g_hookLock);
    return false;
}

DWORD WINAPI AnimationFrameLoaderThread(LPVOID) {
    Log("[texture] animation frame loader thread started\n");
    while (!g_animationWorkerStop.load()) {
        WaitForSingleObject(g_animationWorkerEvent, 250);
        if (g_animationWorkerStop.load()) {
            break;
        }

        AnimationLoadRequest request{};
        while (!g_animationWorkerStop.load() && PopAnimationLoadRequest(request)) {
            ID3D12Device* device = g_device.load();
            if (!device) {
                break;
            }
            device->AddRef();
            if (request.slot) {
                EnsureAnimationFrameLoaded(*request.slot, request.path.c_str(), request.frameIndex, device);
            }
            device->Release();
        }
    }
    Log("[texture] animation frame loader thread stopped\n");
    return 0;
}

void StartAnimationFrameLoader() {
    if (g_animationWorkerThread) {
        return;
    }
    g_animationWorkerStop.store(false);
    if (!g_animationWorkerEvent) {
        g_animationWorkerEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    }
    if (!g_animationWorkerEvent) {
        Log("[texture] animation frame loader event create failed error=%lu\n", GetLastError());
        return;
    }
    g_animationWorkerThread = CreateThread(nullptr, 0, AnimationFrameLoaderThread, nullptr, 0, nullptr);
    if (!g_animationWorkerThread) {
        Log("[texture] animation frame loader thread create failed error=%lu\n", GetLastError());
    }
}

void StopAnimationFrameLoader() {
    g_animationWorkerStop.store(true);
    if (g_animationWorkerEvent) {
        SetEvent(g_animationWorkerEvent);
    }
    if (g_animationWorkerThread) {
        WaitForSingleObject(g_animationWorkerThread, 5000);
        CloseHandle(g_animationWorkerThread);
        g_animationWorkerThread = nullptr;
    }
    if (g_animationWorkerEvent) {
        CloseHandle(g_animationWorkerEvent);
        g_animationWorkerEvent = nullptr;
    }
}

ID3D12Resource* EnsureAnimatedTextureResource(TextureSlot& slot, ID3D12Device* device) {
    if (!device) {
        device = g_device.load();
    }
    if (!device) {
        SetTextureStatus(slot, "%s: waiting for D3D12 device", slot.displayName);
        Log("[%s] cannot create selected animation: no D3D12 device\n", slot.logTag);
        return nullptr;
    }

    std::string requestedPath;
    EnterCriticalSection(&g_hookLock);
    requestedPath = slot.selectedPath;
    if (requestedPath.empty() || !slot.selectedAnimated) {
        LeaveCriticalSection(&g_hookLock);
        return nullptr;
    }
    if (ID3D12Resource* existing = slot.fileResource.load(); existing && slot.activeAnimated && StringEqualsIgnoreCase(slot.activePath, requestedPath)) {
        LeaveCriticalSection(&g_hookLock);
        return existing;
    }
    if (CachedAnimation* cached = FindCachedAnimationLocked(slot, requestedPath)) {
        ActivateCachedAnimationLocked(slot, *cached);
        if (ID3D12Resource* readyFrame = FindAnimationFrameResourceLocked(*cached, slot.animFrameIndex)) {
            slot.fileResource.store(readyFrame);
            SetTextureStatus(slot, "%s: cached animation %ux%u %zu frames", slot.displayName, cached->width, cached->height, AnimationFrameCount(*cached));
            LeaveCriticalSection(&g_hookLock);
            return readyFrame;
        }
        SetTextureStatus(slot, "%s: cached animation metadata %ux%u %zu frames", slot.displayName, cached->width, cached->height, AnimationFrameCount(*cached));
        CW_SKY_VERBOSE_LOG("[%s] animation cache hit path=%s frames=%zu %ux%u fmt=%s(%u) cached=%zu\n",
            slot.logTag,
            requestedPath.c_str(),
            AnimationFrameCount(*cached),
            cached->width,
            cached->height,
            FormatName(cached->format),
            static_cast<unsigned>(cached->format),
            slot.animCache.size());
        QueueAnimationWindowLocked(slot, *cached, slot.animFrameIndex, slot.animDirection, true);
        LeaveCriticalSection(&g_hookLock);
        return nullptr;
    }
    LeaveCriticalSection(&g_hookLock);

    std::vector<std::string> framePaths;
    std::vector<double> frameDurationsMs;
    double fps = kDefaultAnimatedMoonFps;
    AnimationLoopMode loopMode = AnimationLoopMode::Forward;
    size_t startFrame = 0;
    bool randomStart = false;
    if (!LoadAnimatedTextureFrameList(std::filesystem::path(requestedPath), framePaths, fps, frameDurationsMs, loopMode, startFrame, randomStart, true)) {
        SetTextureStatus(slot, "%s: invalid animation frames", slot.displayName);
        Log("[%s] animation frame scan failed path=%s\n", slot.logTag, requestedPath.c_str());
        return nullptr;
    }
    CachedAnimation animation{};
    animation.path = requestedPath;
    animation.framePaths = std::move(framePaths);
    animation.frameMs = 1000.0 / fps;
    animation.loopMode = loopMode;
    animation.startFrame = startFrame;
    animation.randomStart = randomStart;
    animation.frameDurationsMs = std::move(frameDurationsMs);

    SkyImageData firstFrameImage{};
    char loadStatus[128] = {};
    const ULONGLONG metadataStartTick = GetTickCount64();
    if (animation.framePaths.empty() ||
        !LoadSkyImageFile(animation.framePaths[0], firstFrameImage, loadStatus, sizeof(loadStatus))) {
        SetTextureStatus(slot, "%s: frame load failed", slot.displayName);
        Log("[%s] animation frame load failed index=0 path=%s status=%s\n",
            slot.logTag,
            animation.framePaths.empty() ? "" : animation.framePaths[0].c_str(),
            loadStatus);
        return nullptr;
    }
    const DWORD metadataLoadMs = static_cast<DWORD>(std::min<ULONGLONG>(GetTickCount64() - metadataStartTick, MAXDWORD));
    animation.format = firstFrameImage.format;
    animation.mips = static_cast<UINT>(firstFrameImage.mipLevels.size());
    animation.width = firstFrameImage.width;
    animation.height = firstFrameImage.height;
    const UINT64 estimatedSourceBytes = ImageSourceBytes(firstFrameImage) * static_cast<UINT64>(animation.framePaths.size());

    EnterCriticalSection(&g_hookLock);
    if (!slot.selectedAnimated || !StringEqualsIgnoreCase(slot.selectedPath, requestedPath)) {
        CW_SKY_VERBOSE_LOG("[%s] discard loaded animation because selection changed path=%s current=%s\n", slot.logTag, requestedPath.c_str(), slot.selectedPath.c_str());
        LeaveCriticalSection(&g_hookLock);
        return nullptr;
    }
    if (CachedAnimation* cached = FindCachedAnimationLocked(slot, requestedPath)) {
        ActivateCachedAnimationLocked(slot, *cached);
        SetTextureStatus(slot, "%s: cached animation metadata %ux%u %zu frames", slot.displayName, cached->width, cached->height, AnimationFrameCount(*cached));
        QueueAnimationWindowLocked(slot, *cached, slot.animFrameIndex, slot.animDirection, true);
        LeaveCriticalSection(&g_hookLock);
        return nullptr;
    }

    slot.animCache.push_back(std::move(animation));
    ActivateCachedAnimationLocked(slot, slot.animCache.back());
    EnforceAnimationCacheLimitLocked(slot);
    // Re-find after enforcing the cache limit because vector erase can move entries.
    CachedAnimation* cached = FindCachedAnimationLocked(slot, requestedPath);
    if (!cached) {
        DeactivateFileResourceLocked(slot);
        LeaveCriticalSection(&g_hookLock);
        return nullptr;
    }
    SetTextureStatus(slot, "%s: loaded animation metadata %ux%u %zu frames", slot.displayName, cached->width, cached->height, AnimationFrameCount(*cached));
    Log("[%s] runtime animation metadata ready path=%s frames=%zu %ux%u mips=%u fps=%.3f loop=%s start=%zu random=%u fmt=%s(%u) gpuSlots=%zu estimatedSource=%.2f MiB metadataLoadMs=%lu scan=first-frame cached=%zu\n",
        slot.logTag,
        requestedPath.c_str(),
        AnimationFrameCount(*cached),
        cached->width,
        cached->height,
        cached->mips,
        1000.0 / cached->frameMs,
        AnimationLoopModeName(cached->loopMode),
        cached->startFrame,
        cached->randomStart ? 1u : 0u,
        FormatName(cached->format),
        static_cast<unsigned>(cached->format),
        AnimationGpuSlotCapacity(*cached),
        BytesToMiB(estimatedSourceBytes),
        metadataLoadMs,
        slot.animCache.size());

    QueueAnimationWindowLocked(slot, *cached, slot.animFrameIndex, slot.animDirection, true);
    LeaveCriticalSection(&g_hookLock);
    return nullptr;
}

ID3D12Resource* EnsureFileResource(TextureSlot& slot, ID3D12Device* device) {
    if (!device) {
        device = g_device.load();
    }
    if (!device) {
        SetTextureStatus(slot, "%s: waiting for D3D12 device", slot.displayName);
        Log("[%s] cannot create selected texture: no D3D12 device\n", slot.logTag);
        return nullptr;
    }

    std::string requestedPath;
    EnterCriticalSection(&g_hookLock);
    requestedPath = slot.selectedPath;
    if (requestedPath.empty()) {
        LeaveCriticalSection(&g_hookLock);
        return nullptr;
    }
    if (ID3D12Resource* existing = slot.fileResource.load(); existing && StringEqualsIgnoreCase(slot.activePath, requestedPath)) {
        LeaveCriticalSection(&g_hookLock);
        return existing;
    }
    if (CachedTexture* cached = FindCachedTextureLocked(slot, requestedPath)) {
        ActivateCachedTextureLocked(slot, *cached);
        SetTextureStatus(slot, "%s: cached %ux%u", slot.displayName, cached->width, cached->height);
        CW_SKY_VERBOSE_LOG("[%s] cache hit path=%s res=%p %ux%u fmt=%s(%u) cached=%zu\n",
            slot.logTag,
            requestedPath.c_str(),
            cached->resource,
            cached->width,
            cached->height,
            FormatName(cached->format),
            static_cast<unsigned>(cached->format),
            slot.cache.size());
        LeaveCriticalSection(&g_hookLock);
        return cached->resource;
    }
    LeaveCriticalSection(&g_hookLock);

    SkyImageData image{};
    char loadStatus[128] = {};
    if (IsBlankTexturePath(requestedPath)) {
        BuildBlankSkyImage(image);
    } else if (!LoadSkyImageFile(requestedPath, image, loadStatus, sizeof(loadStatus))) {
        SetTextureStatus(slot, "%s: %s", slot.displayName, loadStatus[0] ? loadStatus : "load failed");
        return nullptr;
    }
    UINT mipCount = 0;
    UINT64 uploadSize = 0;
    ID3D12Resource* texture = CreateTextureResourceFromImage(slot, device, image, requestedPath, mipCount, uploadSize);
    if (!texture) {
        return nullptr;
    }

    EnterCriticalSection(&g_hookLock);
    if (!StringEqualsIgnoreCase(slot.selectedPath, requestedPath)) {
        CW_SKY_VERBOSE_LOG("[%s] discard loaded texture because selection changed path=%s current=%s\n", slot.logTag, requestedPath.c_str(), slot.selectedPath.c_str());
        texture->Release();
        LeaveCriticalSection(&g_hookLock);
        return nullptr;
    }
    if (CachedTexture* cached = FindCachedTextureLocked(slot, requestedPath)) {
        texture->Release();
        ActivateCachedTextureLocked(slot, *cached);
        SetTextureStatus(slot, "%s: cached %ux%u", slot.displayName, cached->width, cached->height);
        CW_SKY_VERBOSE_LOG("[%s] cache won race path=%s res=%p %ux%u fmt=%s(%u) cached=%zu\n",
            slot.logTag,
            requestedPath.c_str(),
            cached->resource,
            cached->width,
            cached->height,
            FormatName(cached->format),
            static_cast<unsigned>(cached->format),
            slot.cache.size());
        LeaveCriticalSection(&g_hookLock);
        return cached->resource;
    }

    slot.cache.push_back({ requestedPath, texture, image.format, mipCount, image.width, image.height });
    ActivateCachedTextureLocked(slot, slot.cache.back());
    EnforceCacheLimitLocked(slot);
    SetTextureStatus(slot, "%s: loaded %ux%u mips=%u", slot.displayName, image.width, image.height, mipCount);
    Log("[%s] runtime texture ready %ux%u mips=%u fmt=%s(%u)\n",
        slot.logTag,
        image.width,
        image.height,
        mipCount,
        FormatName(image.format),
        static_cast<unsigned>(image.format));

    LeaveCriticalSection(&g_hookLock);
    return texture;
}

D3D12_SHADER_RESOURCE_VIEW_DESC FileSrvDescFromNative(TextureSlot& slot, const D3D12_SHADER_RESOURCE_VIEW_DESC& nativeDesc) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = nativeDesc;
    desc.Format = SrvFormatForFileTexture(slot.fileFormat, nativeDesc.Format);
    if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
        desc.Texture2D.MostDetailedMip = 0;
        desc.Texture2D.MipLevels = std::max(1u, slot.fileMips);
    }
    return desc;
}

int RewriteTrackedDescriptorsLocked(TextureSlot& slot, ID3D12Resource* replacement) {
    ID3D12Device* device = g_device.load();
    if (!device || !g_realCreateShaderResourceView) {
        SetTextureStatus(slot, "%s: waiting for D3D12 device", slot.displayName);
        return 0;
    }

    int rewritten = 0;
    for (SrvBinding& binding : slot.bindings) {
        if (!binding.dest.ptr || !binding.hasDesc) {
            continue;
        }

        if (replacement) {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc = FileSrvDescFromNative(slot, binding.nativeDesc);
            g_realCreateShaderResourceView(device, replacement, &desc, binding.dest);
        } else if (binding.nativeResource) {
            g_realCreateShaderResourceView(device, binding.nativeResource, &binding.nativeDesc, binding.dest);
        } else {
            continue;
        }
        ++rewritten;
    }

    return rewritten;
}

void InvalidateCachedTextureLocked(TextureSlot& slot, const std::string& path) {
    for (auto it = slot.cache.begin(); it != slot.cache.end(); ++it) {
        if (!StringEqualsIgnoreCase(it->path, path)) {
            continue;
        }

        if (StringEqualsIgnoreCase(slot.activePath, it->path)) {
            RewriteTrackedDescriptorsLocked(slot, nullptr);
            DeactivateFileResourceLocked(slot);
        }
        if (it->resource) {
            it->resource->Release();
        }
        Log("[%s] cache invalidated path=%s cached=%zu\n", slot.logTag, path.c_str(), slot.cache.size() - 1);
        slot.cache.erase(it);
        return;
    }
}

void InvalidateCachedAnimationLocked(TextureSlot& slot, const std::string& path) {
    for (auto it = slot.animCache.begin(); it != slot.animCache.end(); ++it) {
        if (!StringEqualsIgnoreCase(it->path, path)) {
            continue;
        }

        if (slot.activeAnimated && StringEqualsIgnoreCase(slot.activePath, it->path)) {
            RewriteTrackedDescriptorsLocked(slot, nullptr);
            DeactivateFileResourceLocked(slot);
        }
        ReleaseAnimationResources(*it);
        Log("[%s] animation cache invalidated path=%s cached=%zu\n", slot.logTag, path.c_str(), slot.animCache.size() - 1);
        slot.animCache.erase(it);
        return;
    }
}

void ClearTextureSelection(TextureSlot& slot) {
    if (!g_hookLockReady.load()) {
        return;
    }

    EnterCriticalSection(&g_hookLock);
    slot.selectedPath.clear();
    slot.selectedAnimated = false;
    DeactivateFileResourceLocked(slot);
    const bool ready = TextureSwapReadyLocked(slot);
    const int rewritten = ready ? RewriteTrackedDescriptorsLocked(slot, nullptr) : 0;
    if (rewritten > 0) {
        SetTextureStatus(slot, "%s: Native live (%zu descriptors)", slot.displayName, slot.bindings.size());
    } else if (slot.useProof) {
        SetTextureStatus(slot, ready ? "%s: Native live, no descriptors rewritten" : "%s: Native, waiting for proven moon", slot.displayName);
    } else {
        SetTextureStatus(slot, ready ? "%s: Native live, no descriptors rewritten" : "%s: Native, waiting for texture", slot.displayName);
    }
    Log("[%s] selected Native rewritten=%d trackedBindings=%zu\n", slot.logTag, rewritten, slot.bindings.size());
    LeaveCriticalSection(&g_hookLock);
}

void ClearMoonTextureSelection() {
    ClearTextureSelection(g_moonSlot);
}

void ClearMilkywayTextureSelection() {
    ClearTextureSelection(g_milkywaySlot);
}

void ApplyTexturePath(TextureSlot& slot, const char* path) {
    if (!g_hookLockReady.load()) {
        return;
    }
    if (!path || !path[0]) {
        ClearTextureSelection(slot);
        return;
    }

    EnterCriticalSection(&g_hookLock);
    slot.selectedPath = path;
    slot.selectedAnimated = false;
    DeactivateFileResourceLocked(slot);
    SetTextureStatus(slot, "%s: loading selected texture", slot.displayName);
    Log("[%s] selected path=%s trackedBindings=%zu ready=%u proof=%s\n",
        slot.logTag,
        slot.selectedPath.c_str(),
        slot.bindings.size(),
        TextureSwapReadyLocked(slot) ? 1u : 0u,
        slot.useProof ? MoonProofNameLocked() : "n/a");
    LeaveCriticalSection(&g_hookLock);

    ID3D12Resource* fileTexture = EnsureFileResource(slot, g_device.load());

    EnterCriticalSection(&g_hookLock);
    const bool ready = TextureSwapReadyLocked(slot);
    const int rewritten = (fileTexture && ready) ? RewriteTrackedDescriptorsLocked(slot, fileTexture) : 0;
    if (fileTexture) {
        if (rewritten > 0) {
            SetTextureStatus(slot, "%s: selected live (%zu descriptors)", slot.displayName, slot.bindings.size());
        } else if (slot.useProof) {
            SetTextureStatus(slot, ready ? "%s: selected, no descriptors rewritten" : "%s: selected, waiting for proven moon", slot.displayName);
        } else {
            SetTextureStatus(slot, ready ? "%s: selected, no descriptors rewritten" : "%s: selected, waiting for texture", slot.displayName);
        }
    }
    Log("[%s] selected apply rewritten=%d ready=%u file=%p trackedBindings=%zu\n",
        slot.logTag,
        rewritten,
        ready ? 1u : 0u,
        fileTexture,
        slot.bindings.size());
    LeaveCriticalSection(&g_hookLock);
}

void ApplyMoonTexturePath(const char* path) {
    ApplyTexturePath(g_moonSlot, path);
}

void ApplyMilkywayTexturePath(const char* path) {
    ApplyTexturePath(g_milkywaySlot, path);
}

void ApplyAnimationPath(TextureSlot& slot, const char* path) {
    if (!g_hookLockReady.load()) {
        return;
    }
    if (!path || !path[0]) {
        ClearTextureSelection(slot);
        return;
    }

    EnterCriticalSection(&g_hookLock);
    slot.selectedPath = path;
    slot.selectedAnimated = true;
    DeactivateFileResourceLocked(slot);
    SetTextureStatus(slot, "%s: loading selected animation", slot.displayName);
    Log("[%s] selected animation path=%s trackedBindings=%zu ready=%u proof=%s\n",
        slot.logTag,
        slot.selectedPath.c_str(),
        slot.bindings.size(),
        TextureSwapReadyLocked(slot) ? 1u : 0u,
        slot.useProof ? MoonProofNameLocked() : "n/a");
    LeaveCriticalSection(&g_hookLock);
}

void ApplyMoonAnimationPath(const char* path) {
    ApplyAnimationPath(g_moonSlot, path);
}

void ApplyMilkywayAnimationPath(const char* path) {
    ApplyAnimationPath(g_milkywaySlot, path);
}
bool IsNativeResourceLocked(TextureSlot& slot, ID3D12Resource* resource) {
    if (!resource) {
        return false;
    }
    for (ID3D12Resource* item : slot.nativeResources) {
        if (item == resource) {
            return true;
        }
    }
    return false;
}

void TrackNativeResourceLocked(TextureSlot& slot, ID3D12Resource* resource, const char* tag);
void ReleaseBinding(SrvBinding& binding);
void AdoptPendingBindingsLocked(TextureSlot& slot, ID3D12Resource* provenResource);

void ClearTrackedNativeStateLocked(TextureSlot& slot) {
    for (SrvBinding& binding : slot.bindings) {
        ReleaseBinding(binding);
    }
    slot.bindings.clear();

    for (SrvBinding& binding : slot.pendingBindings) {
        ReleaseBinding(binding);
    }
    slot.pendingBindings.clear();

    for (ID3D12Resource* resource : slot.nativeResources) {
        if (resource) {
            resource->Release();
        }
    }
    slot.nativeResources.clear();

    if (slot.primaryNativeResource) {
        slot.primaryNativeResource->Release();
        slot.primaryNativeResource = nullptr;
    }

    slot.nativeLocked.store(false);
    slot.sawUnormSrv = false;
    slot.sawSrgbSrv = false;
    slot.sourceSrvCount = 0;
    slot.copiedSrvCount = 0;
    slot.lastProofLevel = -99;
    slot.contentProofHit = false;
}

void PromoteContentProvenTextureLocked(TextureSlot& slot, ID3D12Resource* resource, const char* tag, uint64_t hash, UINT rowPitch) {
    if (!resource) {
        return;
    }
#if !defined(CW_VERBOSE_SKY_TEXTURE) && !defined(CW_DEV_BUILD)
    (void)hash;
    (void)rowPitch;
#endif

    const bool replacingCandidate = slot.primaryNativeResource && slot.primaryNativeResource != resource;
    if (replacingCandidate) {
        CW_SKY_VERBOSE_LOG("[%s] content proof replacing previous candidate old=%p new=%p\n",
            slot.logTag,
            slot.primaryNativeResource,
            resource);
        ClearTrackedNativeStateLocked(slot);
    }

    if (replacingCandidate || !slot.contentProofHit) {
        slot.contentProofHit = true;
        Log("[%s] content proof matched via %s\n",
            slot.logTag,
            tag ? tag : "native-copy");
        CW_SKY_VERBOSE_LOG("[%s] content proof details hash=0x%llX expected=0x%llX rowPitch=%u res=%p\n",
            slot.logTag,
            static_cast<unsigned long long>(hash),
            static_cast<unsigned long long>(slot.expectedTopMipHash),
            rowPitch,
            resource);
    }

    TrackNativeResourceLocked(slot, resource, tag ? tag : "content-proof");
    AdoptPendingBindingsLocked(slot, resource);
    if (slot.useProof) {
        UpdateMoonProofStateLocked("content-proof");
    }
}

void TrackNativeResourceLocked(TextureSlot& slot, ID3D12Resource* resource, const char* tag) {
    if (!resource) {
        return;
    }

    if (slot.lockFirstNative && slot.primaryNativeResource && slot.primaryNativeResource != resource) {
        const uint32_t rejectIndex = slot.rejectLogCount.fetch_add(1);
        if (rejectIndex < 4) {
            D3D12_RESOURCE_DESC desc = resource->GetDesc();
            CW_SKY_VERBOSE_LOG("[%s] ignored extra native texture-like resource from %s res=%p %llux%u mips=%u fmt=%s(%u) locked=%p\n",
                slot.logTag,
                tag,
                resource,
                static_cast<unsigned long long>(desc.Width),
                desc.Height,
                desc.MipLevels,
                FormatName(desc.Format),
                static_cast<unsigned>(desc.Format),
                slot.primaryNativeResource);
        } else if (rejectIndex == 4) {
            CW_SKY_VERBOSE_LOG("[%s] extra native texture-like resource log cap reached\n", slot.logTag);
        }
        return;
    }

    if (slot.lockFirstNative && !slot.primaryNativeResource) {
        resource->AddRef();
        slot.primaryNativeResource = resource;
        slot.nativeLocked.store(true);
        CW_SKY_VERBOSE_LOG("[%s] native texture locked res=%p via %s\n", slot.logTag, resource, tag);
        if (slot.useProof) {
            UpdateMoonProofStateLocked("native-locked");
        }
    }

    if (IsNativeResourceLocked(slot, resource)) {
        return;
    }

    resource->AddRef();
    slot.nativeResources.push_back(resource);
    const uint32_t index = slot.candidateLogCount.fetch_add(1);
    if (index < 16) {
        D3D12_RESOURCE_DESC desc = resource->GetDesc();
        CW_SKY_VERBOSE_LOG("[%s] native resource from %s res=%p %llux%u mips=%u fmt=%s(%u) candidates=%zu\n",
            slot.logTag,
            tag,
            resource,
            static_cast<unsigned long long>(desc.Width),
            desc.Height,
            desc.MipLevels,
            FormatName(desc.Format),
            static_cast<unsigned>(desc.Format),
            slot.nativeResources.size());
    }
    if (!slot.useProof) {
        LogNativeStack(slot, tag);
    }
}

SrvBinding* FindBindingLocked(TextureSlot& slot, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (!handle.ptr) {
        return nullptr;
    }
    for (SrvBinding& binding : slot.bindings) {
        if (binding.dest.ptr == handle.ptr) {
            return &binding;
        }
    }
    return nullptr;
}

SrvBinding* FindPendingBindingLocked(TextureSlot& slot, D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (!handle.ptr) {
        return nullptr;
    }
    for (SrvBinding& binding : slot.pendingBindings) {
        if (binding.dest.ptr == handle.ptr) {
            return &binding;
        }
    }
    return nullptr;
}

void ReleaseBinding(SrvBinding& binding) {
    if (binding.nativeResource) {
        binding.nativeResource->Release();
        binding.nativeResource = nullptr;
    }
}

bool IsNativeProofSrvDesc(const TextureSlot& slot, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc) {
    return desc &&
           desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
           desc->Texture2D.MostDetailedMip == 0 &&
           desc->Texture2D.MipLevels == slot.nativeMips &&
           (desc->Format == DXGI_FORMAT_BC1_UNORM || desc->Format == DXGI_FORMAT_BC1_UNORM_SRGB);
}

void TrackPendingSourceSrvLocked(TextureSlot& slot,
                                 ID3D12Resource* nativeResource,
                                 const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                 D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                 const char* tag) {
    if (!slot.useProof || slot.contentProofHit || !nativeResource || !dest.ptr || !IsNativeProofSrvDesc(slot, desc)) {
        return;
    }

    SrvBinding* binding = FindPendingBindingLocked(slot, dest);
    if (!binding && slot.pendingBindings.size() >= 64) {
        ReleaseBinding(slot.pendingBindings.front());
        slot.pendingBindings.erase(slot.pendingBindings.begin());
    }

    if (!binding) {
        slot.pendingBindings.push_back({});
        binding = &slot.pendingBindings.back();
        nativeResource->AddRef();
        binding->nativeResource = nativeResource;
    } else if (binding->nativeResource != nativeResource) {
        ReleaseBinding(*binding);
        nativeResource->AddRef();
        binding->nativeResource = nativeResource;
    }

    binding->dest = dest;
    binding->hasDesc = true;
    binding->nativeDesc = *desc;
    binding->copied = false;

    const uint32_t index = slot.pendingDescriptorLogCount.fetch_add(1);
    if (index < 8) {
        CW_SKY_VERBOSE_LOG("[%s] pending source SRV %s res=%p desc=0x%llX fmt=%s(%u) pending=%zu\n",
            slot.logTag,
            tag,
            nativeResource,
            static_cast<unsigned long long>(dest.ptr),
            FormatName(desc->Format),
            static_cast<unsigned>(desc->Format),
            slot.pendingBindings.size());
    } else if (index == 8) {
        CW_SKY_VERBOSE_LOG("[%s] pending source SRV log cap reached\n", slot.logTag);
    }
}

void TrackBindingLocked(TextureSlot& slot,
                        ID3D12Resource* nativeResource,
                        const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                        D3D12_CPU_DESCRIPTOR_HANDLE dest,
                        bool copied,
                        const char* tag) {
    if (!nativeResource || !dest.ptr) {
        return;
    }

    SrvBinding* binding = FindBindingLocked(slot, dest);
    if (!binding && slot.bindings.size() >= slot.maxBindings) {
        static bool loggedMoonCap = false;
        static bool loggedMilkywayCap = false;
        bool& loggedCap = slot.useProof ? loggedMoonCap : loggedMilkywayCap;
        if (!loggedCap) {
            CW_SKY_VERBOSE_LOG("[%s] tracked descriptor cap reached (%zu)\n", slot.logTag, slot.bindings.size());
            loggedCap = true;
        }
        return;
    }

    const bool isNew = binding == nullptr;
    if (isNew) {
        slot.bindings.push_back({});
        binding = &slot.bindings.back();
        nativeResource->AddRef();
        binding->nativeResource = nativeResource;
    } else if (binding->nativeResource != nativeResource) {
        if (binding->nativeResource) {
            binding->nativeResource->Release();
        }
        nativeResource->AddRef();
        binding->nativeResource = nativeResource;
    }

    binding->dest = dest;
    binding->hasDesc = desc != nullptr;
    binding->nativeDesc = desc ? *desc : D3D12_SHADER_RESOURCE_VIEW_DESC{};
    binding->copied = copied;

    if (slot.useProof) {
        if (isNew) {
            if (copied) {
                ++slot.copiedSrvCount;
            } else {
                ++slot.sourceSrvCount;
            }
        }

        if (desc && desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
            desc->Texture2D.MostDetailedMip == 0 &&
            desc->Texture2D.MipLevels == slot.nativeMips) {
            if (desc->Format == DXGI_FORMAT_BC1_UNORM) {
                slot.sawUnormSrv = true;
            } else if (desc->Format == DXGI_FORMAT_BC1_UNORM_SRGB) {
                slot.sawSrgbSrv = true;
            }
        }

        UpdateMoonProofStateLocked(copied ? "copied-srv" : "source-srv");
        return;
    }

    const uint32_t index = slot.descriptorLogCount.fetch_add(1);
    if (index < 12 || isNew) {
        CW_SKY_VERBOSE_LOG("[%s] descriptor %s desc=0x%llX copied=%u fmt=%s(%u) bindings=%zu\n",
            slot.logTag,
            tag,
            static_cast<unsigned long long>(dest.ptr),
            copied ? 1u : 0u,
            desc ? FormatName(desc->Format) : "null",
            desc ? static_cast<unsigned>(desc->Format) : 0u,
            slot.bindings.size());
    } else if (index == 12) {
        CW_SKY_VERBOSE_LOG("[%s] descriptor log cap reached\n", slot.logTag);
    }
}

void AdoptPendingBindingsLocked(TextureSlot& slot, ID3D12Resource* provenResource) {
    if (!slot.useProof || !provenResource || slot.pendingBindings.empty()) {
        return;
    }

    size_t adopted = 0;
    for (SrvBinding& pending : slot.pendingBindings) {
        if (pending.nativeResource != provenResource || !pending.hasDesc) {
            continue;
        }
        TrackBindingLocked(slot, provenResource, &pending.nativeDesc, pending.dest, false, "pending-source-srv");
        ++adopted;
    }

    for (SrvBinding& pending : slot.pendingBindings) {
        ReleaseBinding(pending);
    }
    slot.pendingBindings.clear();

    if (adopted > 0) {
        CW_SKY_VERBOSE_LOG("[%s] adopted %zu pending source SRV(s) after content proof\n", slot.logTag, adopted);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorAt(UINT rangeCount,
                                         const D3D12_CPU_DESCRIPTOR_HANDLE* starts,
                                         const UINT* sizes,
                                         UINT logicalIndex,
                                         UINT increment) {
    D3D12_CPU_DESCRIPTOR_HANDLE result{};
    if (!starts || increment == 0) {
        return result;
    }

    UINT base = 0;
    for (UINT range = 0; range < rangeCount; ++range) {
        const UINT rangeSize = sizes ? sizes[range] : 1;
        if (logicalIndex < base + rangeSize) {
            result.ptr = starts[range].ptr + static_cast<SIZE_T>(logicalIndex - base) * increment;
            return result;
        }
        base += rangeSize;
    }
    return result;
}

UINT TotalDescriptorCount(UINT rangeCount, const UINT* sizes) {
    if (!sizes) {
        return rangeCount;
    }

    UINT total = 0;
    for (UINT i = 0; i < rangeCount; ++i) {
        total += sizes[i];
    }
    return total;
}

void MaybeTrackCopiedDescriptor(TextureSlot& slot,
                                ID3D12Device* device,
                                D3D12_CPU_DESCRIPTOR_HANDLE source,
                                D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                const char* tag) {
    if (!device || !source.ptr || !dest.ptr || source.ptr == dest.ptr || !g_hookLockReady.load()) {
        return;
    }

    ID3D12Resource* fileTexture = nullptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC nativeDesc{};
    bool hasDesc = false;
    EnterCriticalSection(&g_hookLock);
    SrvBinding* sourceBinding = FindBindingLocked(slot, source);
    if (sourceBinding && sourceBinding->nativeResource && sourceBinding->hasDesc) {
        ID3D12Resource* nativeResource = sourceBinding->nativeResource;
        nativeResource->AddRef();
        nativeDesc = sourceBinding->nativeDesc;
        hasDesc = true;
        TrackBindingLocked(slot, nativeResource, &nativeDesc, dest, true, tag);
        nativeResource->Release();
        fileTexture = (!slot.selectedPath.empty() && TextureSwapReadyLocked(slot)) ? slot.fileResource.load() : nullptr;
        if (fileTexture) {
            fileTexture->AddRef();
            if (slot.rewriteAllOnCopy) {
                const int rewritten = RewriteTrackedDescriptorsLocked(slot, fileTexture);
                SetTextureStatus(slot, "%s: selected live (%zu descriptors)", slot.displayName, slot.bindings.size());
                CW_SKY_VERBOSE_LOG("[%s] auto rewrite after %s rewritten=%d file=%p\n", slot.logTag, tag, rewritten, fileTexture);
            }
        }

        const uint32_t index = slot.descriptorCopyLogCount.fetch_add(1);
        if (index < 4) {
            CW_SKY_VERBOSE_LOG("[%s] %s copied descriptor src=0x%llX dst=0x%llX\n",
                slot.logTag,
                tag,
                static_cast<unsigned long long>(source.ptr),
                static_cast<unsigned long long>(dest.ptr));
        } else if (index == 4) {
            CW_SKY_VERBOSE_LOG("[%s] descriptor copy log cap reached\n", slot.logTag);
        }
    }
    LeaveCriticalSection(&g_hookLock);

    if (fileTexture && hasDesc && g_realCreateShaderResourceView) {
        D3D12_SHADER_RESOURCE_VIEW_DESC fileDesc = FileSrvDescFromNative(slot, nativeDesc);
        g_realCreateShaderResourceView(device, fileTexture, &fileDesc, dest);
        fileTexture->Release();
    }
}

bool IsFullTopMipCopyBox(TextureSlot& slot, const D3D12_BOX* box) {
    if (!box) {
        return true;
    }
    return box->left == 0 &&
           box->top == 0 &&
           box->front == 0 &&
           box->right == slot.nativeWidth &&
           box->bottom == slot.nativeHeight &&
           (box->back == 0 || box->back == 1);
}

bool IsCpuReadableUploadResource(ID3D12Resource* resource) {
    if (!resource) {
        return false;
    }

    D3D12_HEAP_PROPERTIES props{};
    D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
    if (FAILED(resource->GetHeapProperties(&props, &flags))) {
        return false;
    }

    return props.Type == D3D12_HEAP_TYPE_UPLOAD ||
           props.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE ||
           props.CPUPageProperty == D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
}

bool TryHashPlacedTextureUpload(TextureSlot& slot,
                                ID3D12Resource* source,
                                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,
                                uint64_t* outHash,
                                HRESULT* outHr) {
    if (outHash) {
        *outHash = 0;
    }
    if (outHr) {
        *outHr = E_FAIL;
    }
    if (!source || !outHash) {
        return false;
    }

    const UINT blocksWide = (slot.nativeWidth + 3u) / 4u;
    const UINT blockRows = (slot.nativeHeight + 3u) / 4u;
    const UINT tightRowBytes = blocksWide * kBc1BlockBytes;
    const UINT rowPitch = footprint.Footprint.RowPitch;
    if (rowPitch < tightRowBytes) {
        return false;
    }

    const uint64_t readSize = static_cast<uint64_t>(rowPitch) * blockRows;
    void* mappedBase = nullptr;
    D3D12_RANGE readRange{};
    readRange.Begin = static_cast<SIZE_T>(footprint.Offset);
    readRange.End = static_cast<SIZE_T>(footprint.Offset + readSize);
    HRESULT hr = E_FAIL;
    __try {
        hr = source->Map(0, &readRange, &mappedBase);
        if (SUCCEEDED(hr) && mappedBase) {
            const uint8_t* mapped = static_cast<const uint8_t*>(mappedBase) + footprint.Offset;
            *outHash = Fnv1a64Rows(mapped, rowPitch, tightRowBytes, blockRows);
            D3D12_RANGE writtenRange{ 0, 0 };
            source->Unmap(0, &writtenRange);
        }
    } __except (LogSkyExceptionFilter("SkyTextureProof/NativeCopyMap", GetExceptionInformation())) {
        hr = E_FAIL;
        mappedBase = nullptr;
    }

    if (outHr) {
        *outHr = hr;
    }
    return SUCCEEDED(hr) && mappedBase != nullptr;
}

void TryProveTextureFromNativeCopy(TextureSlot& slot,
                                   const D3D12_TEXTURE_COPY_LOCATION* dst,
                                   const D3D12_TEXTURE_COPY_LOCATION* src,
                                   const D3D12_BOX* srcBox) {
    if (!dst || !src ||
        dst->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ||
        src->Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ||
        dst->SubresourceIndex != 0 ||
        !dst->pResource ||
        !src->pResource ||
        !IsFullTopMipCopyBox(slot, srcBox)) {
        return;
    }

    D3D12_RESOURCE_DESC dstDesc = dst->pResource->GetDesc();
    if (!IsNativeDesc(slot, &dstDesc)) {
        return;
    }

    bool trackedNativeCandidate = false;
    bool alreadyProven = false;
    if (g_hookLockReady.load()) {
        EnterCriticalSection(&g_hookLock);
        trackedNativeCandidate = IsNativeResourceLocked(slot, dst->pResource);
        alreadyProven = slot.contentProofHit;
        LeaveCriticalSection(&g_hookLock);
    }
    if (alreadyProven) {
        return;
    }
    if (!trackedNativeCandidate) {
        const uint32_t count = slot.contentRejectLogCount.fetch_add(1);
        if (count < 4) {
            CW_SKY_VERBOSE_LOG("[%s] content proof skipped untracked native-sized copy dest=%p %llux%u mips=%u fmt=%s(%u)\n",
                slot.logTag,
                dst->pResource,
                static_cast<unsigned long long>(dstDesc.Width),
                dstDesc.Height,
                dstDesc.MipLevels,
                FormatName(dstDesc.Format),
                static_cast<unsigned>(dstDesc.Format));
        }
        return;
    }

    const D3D12_SUBRESOURCE_FOOTPRINT& footprint = src->PlacedFootprint.Footprint;
    if (footprint.Width != slot.nativeWidth ||
        footprint.Height != slot.nativeHeight ||
        footprint.Depth != 1 ||
        !IsBc1Format(static_cast<DXGI_FORMAT>(footprint.Format))) {
        return;
    }

    if (!IsCpuReadableUploadResource(src->pResource)) {
        const uint32_t count = slot.contentRejectLogCount.fetch_add(1);
        if (count < 4) {
            D3D12_RESOURCE_DESC srcDesc = src->pResource->GetDesc();
            CW_SKY_VERBOSE_LOG("[%s] content proof skipped non-upload source=%p dim=%u width=%llu\n",
                slot.logTag,
                src->pResource,
                static_cast<unsigned>(srcDesc.Dimension),
                static_cast<unsigned long long>(srcDesc.Width));
        }
        return;
    }

    HRESULT hr = E_FAIL;
    uint64_t hash = 0;
    if (!TryHashPlacedTextureUpload(slot, src->pResource, src->PlacedFootprint, &hash, &hr)) {
        const uint32_t count = slot.contentRejectLogCount.fetch_add(1);
        if (count < 8) {
            CW_SKY_VERBOSE_LOG("[%s] content proof native map failed hr=0x%08X source=%p dest=%p offset=%llu rowPitch=%u\n",
                slot.logTag,
                static_cast<unsigned>(hr),
                src->pResource,
                dst->pResource,
                static_cast<unsigned long long>(src->PlacedFootprint.Offset),
                src->PlacedFootprint.Footprint.RowPitch);
        }
        return;
    }

    if (slot.expectedTopMipHash == 0) {
        const uint32_t count = slot.contentRejectLogCount.fetch_add(1);
        if (count < 6) {
            CW_SKY_VERBOSE_LOG("[%s] content proof observed hash=0x%llX dest=%p offset=%llu rowPitch=%u (not enforced yet)\n",
                slot.logTag,
                static_cast<unsigned long long>(hash),
                dst->pResource,
                static_cast<unsigned long long>(src->PlacedFootprint.Offset),
                src->PlacedFootprint.Footprint.RowPitch);
        }
        EnterCriticalSection(&g_hookLock);
        PromoteContentProvenTextureLocked(slot, dst->pResource, "CopyTextureRegionObserved", hash, src->PlacedFootprint.Footprint.RowPitch);
        LeaveCriticalSection(&g_hookLock);
        return;
    }

    if (hash != slot.expectedTopMipHash) {
        const uint32_t count = slot.contentRejectLogCount.fetch_add(1);
        if (count < 12) {
            CW_SKY_VERBOSE_LOG("[%s] content proof rejected hash=0x%llX expected=0x%llX dest=%p offset=%llu rowPitch=%u\n",
                slot.logTag,
                static_cast<unsigned long long>(hash),
                static_cast<unsigned long long>(slot.expectedTopMipHash),
                dst->pResource,
                static_cast<unsigned long long>(src->PlacedFootprint.Offset),
                src->PlacedFootprint.Footprint.RowPitch);
        } else if (count == 12) {
            CW_SKY_VERBOSE_LOG("[%s] content proof reject log cap reached\n", slot.logTag);
        }
        return;
    }

    EnterCriticalSection(&g_hookLock);
    PromoteContentProvenTextureLocked(slot, dst->pResource, "CopyTextureRegion", hash, src->PlacedFootprint.Footprint.RowPitch);
    LeaveCriticalSection(&g_hookLock);
}

void TrackCommittedResourceHits(const bool* hits, void** resource, const char* tag) {
    if (!resource || !*resource || !g_hookLockReady.load()) {
        return;
    }

    auto* d3dResource = reinterpret_cast<ID3D12Resource*>(*resource);
    EnterCriticalSection(&g_hookLock);
    for (size_t i = 0; i < sizeof(g_textureSlots) / sizeof(g_textureSlots[0]); ++i) {
        if (hits[i]) {
            TrackNativeResourceLocked(*g_textureSlots[i], d3dResource, tag);
        }
    }
    LeaveCriticalSection(&g_hookLock);
}

bool HookVtableEntry(void** vtable, int index, void* detour, void** original, const char* name);
void STDMETHODCALLTYPE HookCopyTextureRegion(ID3D12GraphicsCommandList* self,
                                             const D3D12_TEXTURE_COPY_LOCATION* dst,
                                             UINT dstX,
                                             UINT dstY,
                                             UINT dstZ,
                                             const D3D12_TEXTURE_COPY_LOCATION* src,
                                             const D3D12_BOX* srcBox);

void TryInstallCommandListHooks(ID3D12GraphicsCommandList* list) {
    if (!list) {
        return;
    }

    bool expected = false;
    if (!g_commandListHooksInstalled.compare_exchange_strong(expected, true)) {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(list);
    CW_SKY_VERBOSE_LOG("[moon-main] installing ID3D12GraphicsCommandList hooks list=%p vtable=%p\n", list, vtable);
    if (!HookVtableEntry(vtable, 16, reinterpret_cast<void*>(&HookCopyTextureRegion), reinterpret_cast<void**>(&g_realCopyTextureRegion), "CopyTextureRegion")) {
        g_commandListHooksInstalled.store(false);
    }
}

HRESULT STDMETHODCALLTYPE HookCreateCommandList(ID3D12Device* self,
                                                UINT nodeMask,
                                                D3D12_COMMAND_LIST_TYPE type,
                                                ID3D12CommandAllocator* allocator,
                                                ID3D12PipelineState* initialState,
                                                REFIID riid,
                                                void** commandList) {
    HRESULT hr = g_realCreateCommandList(self, nodeMask, type, allocator, initialState, riid, commandList);
    if (SUCCEEDED(hr) && commandList && *commandList && !g_skyTextureHooksDisabled.load()) {
        ID3D12GraphicsCommandList* list = nullptr;
        if (SUCCEEDED(reinterpret_cast<IUnknown*>(*commandList)->QueryInterface(__uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&list))) && list) {
            TryInstallCommandListHooks(list);
            list->Release();
        }
    }
    return hr;
}

void STDMETHODCALLTYPE HookCopyTextureRegion(ID3D12GraphicsCommandList* self,
                                             const D3D12_TEXTURE_COPY_LOCATION* dst,
                                             UINT dstX,
                                             UINT dstY,
                                             UINT dstZ,
                                             const D3D12_TEXTURE_COPY_LOCATION* src,
                                             const D3D12_BOX* srcBox) {
    if (!g_skyTextureHooksDisabled.load() && g_hookLockReady.load()) {
        __try {
            if (dstX == 0 && dstY == 0 && dstZ == 0) {
                for (TextureSlot* slot : g_textureSlots) {
                    TryProveTextureFromNativeCopy(*slot, dst, src, srcBox);
                }
            }
        } __except (LogSkyExceptionFilter("HookCopyTextureRegion/proof", GetExceptionInformation())) {
        }
    }

    g_realCopyTextureRegion(self, dst, dstX, dstY, dstZ, src, srcBox);
}

HRESULT STDMETHODCALLTYPE HookCreateCommittedResource(ID3D12Device* self,
                                                      const D3D12_HEAP_PROPERTIES* heapProperties,
                                                      D3D12_HEAP_FLAGS heapFlags,
                                                      const D3D12_RESOURCE_DESC* desc,
                                                      D3D12_RESOURCE_STATES initialState,
                                                      const D3D12_CLEAR_VALUE* optimizedClearValue,
                                                      REFIID riidResource,
                                                      void** resource) {
    if (g_skyTextureHooksDisabled.load()) {
        return g_realCreateCommittedResource(self, heapProperties, heapFlags, desc, initialState, optimizedClearValue, riidResource, resource);
    }
    bool hits[sizeof(g_textureSlots) / sizeof(g_textureSlots[0])] = {};
    __try {
        for (size_t i = 0; i < sizeof(g_textureSlots) / sizeof(g_textureSlots[0]); ++i) {
            hits[i] = ShouldTrackNativeDesc(*g_textureSlots[i], desc);
        }
    } __except (LogSkyExceptionFilter("HookCreateCommittedResource/pre", GetExceptionInformation())) {
    }
    HRESULT hr = g_realCreateCommittedResource(self, heapProperties, heapFlags, desc, initialState, optimizedClearValue, riidResource, resource);
    if (SUCCEEDED(hr) && !g_skyTextureHooksDisabled.load()) {
        __try {
            TrackCommittedResourceHits(hits, resource, "CreateCommittedResource");
        } __except (LogSkyExceptionFilter("HookCreateCommittedResource/post", GetExceptionInformation())) {
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateCommittedResource1(ID3D12Device4* self,
                                                       const D3D12_HEAP_PROPERTIES* heapProperties,
                                                       D3D12_HEAP_FLAGS heapFlags,
                                                       const D3D12_RESOURCE_DESC* desc,
                                                       D3D12_RESOURCE_STATES initialState,
                                                       const D3D12_CLEAR_VALUE* optimizedClearValue,
                                                       ID3D12ProtectedResourceSession* protectedSession,
                                                       REFIID riidResource,
                                                       void** resource) {
    if (g_skyTextureHooksDisabled.load()) {
        return g_realCreateCommittedResource1(self, heapProperties, heapFlags, desc, initialState, optimizedClearValue, protectedSession, riidResource, resource);
    }
    bool hits[sizeof(g_textureSlots) / sizeof(g_textureSlots[0])] = {};
    __try {
        for (size_t i = 0; i < sizeof(g_textureSlots) / sizeof(g_textureSlots[0]); ++i) {
            hits[i] = ShouldTrackNativeDesc(*g_textureSlots[i], desc);
        }
    } __except (LogSkyExceptionFilter("HookCreateCommittedResource1/pre", GetExceptionInformation())) {
    }
    HRESULT hr = g_realCreateCommittedResource1(self, heapProperties, heapFlags, desc, initialState, optimizedClearValue, protectedSession, riidResource, resource);
    if (SUCCEEDED(hr) && !g_skyTextureHooksDisabled.load()) {
        __try {
            TrackCommittedResourceHits(hits, resource, "CreateCommittedResource1");
        } __except (LogSkyExceptionFilter("HookCreateCommittedResource1/post", GetExceptionInformation())) {
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateCommittedResource2(ID3D12Device8* self,
                                                       const D3D12_HEAP_PROPERTIES* heapProperties,
                                                       D3D12_HEAP_FLAGS heapFlags,
                                                       const D3D12_RESOURCE_DESC1* desc,
                                                       D3D12_RESOURCE_STATES initialState,
                                                       const D3D12_CLEAR_VALUE* optimizedClearValue,
                                                       ID3D12ProtectedResourceSession* protectedSession,
                                                       REFIID riidResource,
                                                       void** resource) {
    if (g_skyTextureHooksDisabled.load()) {
        return g_realCreateCommittedResource2(self, heapProperties, heapFlags, desc, initialState, optimizedClearValue, protectedSession, riidResource, resource);
    }
    bool hits[sizeof(g_textureSlots) / sizeof(g_textureSlots[0])] = {};
    __try {
        for (size_t i = 0; i < sizeof(g_textureSlots) / sizeof(g_textureSlots[0]); ++i) {
            hits[i] = ShouldTrackNativeDesc1(*g_textureSlots[i], desc);
        }
    } __except (LogSkyExceptionFilter("HookCreateCommittedResource2/pre", GetExceptionInformation())) {
    }
    HRESULT hr = g_realCreateCommittedResource2(self, heapProperties, heapFlags, desc, initialState, optimizedClearValue, protectedSession, riidResource, resource);
    if (SUCCEEDED(hr) && !g_skyTextureHooksDisabled.load()) {
        __try {
            TrackCommittedResourceHits(hits, resource, "CreateCommittedResource2");
        } __except (LogSkyExceptionFilter("HookCreateCommittedResource2/post", GetExceptionInformation())) {
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateCommittedResource3(ID3D12Device10* self,
                                                       const D3D12_HEAP_PROPERTIES* heapProperties,
                                                       D3D12_HEAP_FLAGS heapFlags,
                                                       const D3D12_RESOURCE_DESC1* desc,
                                                       D3D12_BARRIER_LAYOUT initialLayout,
                                                       const D3D12_CLEAR_VALUE* optimizedClearValue,
                                                       ID3D12ProtectedResourceSession* protectedSession,
                                                       UINT32 numCastableFormats,
                                                       const DXGI_FORMAT* castableFormats,
                                                       REFIID riidResource,
                                                       void** resource) {
    if (g_skyTextureHooksDisabled.load()) {
        return g_realCreateCommittedResource3(self, heapProperties, heapFlags, desc, initialLayout, optimizedClearValue, protectedSession, numCastableFormats, castableFormats, riidResource, resource);
    }
    bool hits[sizeof(g_textureSlots) / sizeof(g_textureSlots[0])] = {};
    __try {
        for (size_t i = 0; i < sizeof(g_textureSlots) / sizeof(g_textureSlots[0]); ++i) {
            hits[i] = ShouldTrackNativeDesc1(*g_textureSlots[i], desc);
        }
    } __except (LogSkyExceptionFilter("HookCreateCommittedResource3/pre", GetExceptionInformation())) {
    }
    HRESULT hr = g_realCreateCommittedResource3(self, heapProperties, heapFlags, desc, initialLayout, optimizedClearValue, protectedSession, numCastableFormats, castableFormats, riidResource, resource);
    if (SUCCEEDED(hr) && !g_skyTextureHooksDisabled.load()) {
        __try {
            TrackCommittedResourceHits(hits, resource, "CreateCommittedResource3");
        } __except (LogSkyExceptionFilter("HookCreateCommittedResource3/post", GetExceptionInformation())) {
        }
    }
    return hr;
}

void STDMETHODCALLTYPE HookCreateShaderResourceView(ID3D12Device* self,
                                                    ID3D12Resource* resource,
                                                    const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor) {
    if (g_skyTextureHooksDisabled.load()) {
        g_realCreateShaderResourceView(self, resource, desc, destDescriptor);
        return;
    }
    TextureSlot* hitSlot = nullptr;
    bool active = false;
    D3D12_RESOURCE_DESC resourceDesc{};
    if (resource && g_hookLockReady.load()) {
        resourceDesc = resource->GetDesc();
        EnterCriticalSection(&g_hookLock);
        for (TextureSlot* slot : g_textureSlots) {
            if (!IsNativeResourceLocked(*slot, resource)) {
                continue;
            }
            TrackBindingLocked(*slot, resource, desc, destDescriptor, false, "CreateShaderResourceView");
            active = !slot->selectedPath.empty() && TextureSwapReadyLocked(*slot);
            hitSlot = slot;
            break;
        }
        if (!hitSlot) {
            for (TextureSlot* slot : g_textureSlots) {
                if (!slot->useProof || !IsNativeDesc(*slot, &resourceDesc)) {
                    continue;
                }
                TrackPendingSourceSrvLocked(*slot, resource, desc, destDescriptor, "CreateShaderResourceView");
                break;
            }
        }
        LeaveCriticalSection(&g_hookLock);
    }

    ID3D12Resource* srvResource = resource;
    D3D12_SHADER_RESOURCE_VIEW_DESC fileSrvDesc{};
    const D3D12_SHADER_RESOURCE_VIEW_DESC* srvDesc = desc;
    ID3D12Resource* fileResource = nullptr;
    if (hitSlot && active) {
        __try {
            fileResource = hitSlot->selectedAnimated
                ? EnsureAnimatedTextureResource(*hitSlot, self)
                : EnsureFileResource(*hitSlot, self);
        } __except (LogSkyExceptionFilter("HookCreateShaderResourceView/EnsureFileResource", GetExceptionInformation())) {
            fileResource = nullptr;
        }
    }
    if (fileResource && desc) {
        fileSrvDesc = FileSrvDescFromNative(*hitSlot, *desc);
        srvResource = fileResource;
        srvDesc = &fileSrvDesc;
    }

    g_realCreateShaderResourceView(self, srvResource, srvDesc, destDescriptor);

    if (hitSlot) {
        static std::atomic<uint32_t> s_srvLogCount{ 0 };
        const uint32_t srvLogIndex = hitSlot->useProof ? s_srvLogCount.fetch_add(1) : 0;
        if (!hitSlot->useProof || srvLogIndex < 4) {
            Log("[%s] CreateShaderResourceView res=%p %llux%u mips=%u fmt=%s(%u) srvFmt=%s(%u) desc=0x%llX replacement=%u\n",
                hitSlot->logTag,
                resource,
                static_cast<unsigned long long>(resourceDesc.Width),
                resourceDesc.Height,
                resourceDesc.MipLevels,
                FormatName(resourceDesc.Format),
                static_cast<unsigned>(resourceDesc.Format),
                srvDesc ? FormatName(srvDesc->Format) : "null",
                srvDesc ? static_cast<unsigned>(srvDesc->Format) : 0u,
                static_cast<unsigned long long>(destDescriptor.ptr),
                fileResource ? 1u : 0u);
        } else if (srvLogIndex == 4) {
            Log("[%s] CreateShaderResourceView log cap reached\n", hitSlot->logTag);
        }
    }
}

void STDMETHODCALLTYPE HookCopyDescriptors(ID3D12Device* self,
                                           UINT numDestDescriptorRanges,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE* destDescriptorRangeStarts,
                                           const UINT* destDescriptorRangeSizes,
                                           UINT numSrcDescriptorRanges,
                                           const D3D12_CPU_DESCRIPTOR_HANDLE* srcDescriptorRangeStarts,
                                           const UINT* srcDescriptorRangeSizes,
                                           D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapsType) {
    g_realCopyDescriptors(self,
        numDestDescriptorRanges,
        destDescriptorRangeStarts,
        destDescriptorRangeSizes,
        numSrcDescriptorRanges,
        srcDescriptorRangeStarts,
        srcDescriptorRangeSizes,
        descriptorHeapsType);

    if (descriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        return;
    }
    if (g_skyTextureHooksDisabled.load()) {
        return;
    }

    __try {
        const UINT increment = self->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const UINT totalDest = TotalDescriptorCount(numDestDescriptorRanges, destDescriptorRangeSizes);
        const UINT totalSrc = TotalDescriptorCount(numSrcDescriptorRanges, srcDescriptorRangeSizes);
        const UINT total = std::min(totalDest, totalSrc);
        for (UINT i = 0; i < total; ++i) {
            const D3D12_CPU_DESCRIPTOR_HANDLE source = DescriptorAt(numSrcDescriptorRanges, srcDescriptorRangeStarts, srcDescriptorRangeSizes, i, increment);
            const D3D12_CPU_DESCRIPTOR_HANDLE dest = DescriptorAt(numDestDescriptorRanges, destDescriptorRangeStarts, destDescriptorRangeSizes, i, increment);
            for (TextureSlot* slot : g_textureSlots) {
                MaybeTrackCopiedDescriptor(*slot, self, source, dest, "CopyDescriptors");
            }
        }
    } __except (LogSkyExceptionFilter("HookCopyDescriptors/post", GetExceptionInformation())) {
    }
}

void STDMETHODCALLTYPE HookCopyDescriptorsSimple(ID3D12Device* self,
                                                 UINT numDescriptors,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE destDescriptorRangeStart,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE srcDescriptorRangeStart,
                                                 D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapsType) {
    g_realCopyDescriptorsSimple(self,
        numDescriptors,
        destDescriptorRangeStart,
        srcDescriptorRangeStart,
        descriptorHeapsType);

    if (descriptorHeapsType != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        return;
    }
    if (g_skyTextureHooksDisabled.load()) {
        return;
    }

    __try {
        const UINT increment = self->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (UINT i = 0; i < numDescriptors; ++i) {
            D3D12_CPU_DESCRIPTOR_HANDLE source{};
            D3D12_CPU_DESCRIPTOR_HANDLE dest{};
            source.ptr = srcDescriptorRangeStart.ptr + static_cast<SIZE_T>(i) * increment;
            dest.ptr = destDescriptorRangeStart.ptr + static_cast<SIZE_T>(i) * increment;
            for (TextureSlot* slot : g_textureSlots) {
                MaybeTrackCopiedDescriptor(*slot, self, source, dest, "CopyDescriptorsSimple");
            }
        }
    } __except (LogSkyExceptionFilter("HookCopyDescriptorsSimple/post", GetExceptionInformation())) {
    }
}
bool HookVtableEntry(void** vtable, int index, void* detour, void** original, const char* name) {
    MH_STATUS createStatus = MH_CreateHook(vtable[index], detour, original);
    if (createStatus != MH_OK && createStatus != MH_ERROR_ALREADY_CREATED) {
        Log("[moon-main] hook create failed %s index=%d target=%p status=%d\n", name, index, vtable[index], static_cast<int>(createStatus));
        return false;
    }

    MH_STATUS enableStatus = MH_EnableHook(vtable[index]);
    if (enableStatus != MH_OK && enableStatus != MH_ERROR_ENABLED) {
        Log("[moon-main] hook enable failed %s index=%d target=%p status=%d\n", name, index, vtable[index], static_cast<int>(enableStatus));
        return false;
    }

    CW_SKY_VERBOSE_LOG("[moon-main] hooked %s index=%d target=%p\n", name, index, vtable[index]);
    return true;
}

void InstallDeviceHooksBody(ID3D12Device* device) {
    if (g_device.load()) {
        CW_SKY_VERBOSE_LOG("[moon-main] InstallDeviceHooks skipped, device already captured\n");
        return;
    }

    bool expected = false;
    if (!g_deviceHooksInstalled.compare_exchange_strong(expected, true)) {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    Log("[moon-main] D3D12 device hooks installing\n");
    CW_SKY_VERBOSE_LOG("[moon-main] installing ID3D12Device hooks device=%p vtable=%p\n", device, vtable);

    device->AddRef();
    g_device.store(device);

    HookVtableEntry(vtable, 12, reinterpret_cast<void*>(&HookCreateCommandList), reinterpret_cast<void**>(&g_realCreateCommandList), "CreateCommandList");
    HookVtableEntry(vtable, 18, reinterpret_cast<void*>(&HookCreateShaderResourceView), reinterpret_cast<void**>(&g_realCreateShaderResourceView), "CreateShaderResourceView");
    HookVtableEntry(vtable, 23, reinterpret_cast<void*>(&HookCopyDescriptors), reinterpret_cast<void**>(&g_realCopyDescriptors), "CopyDescriptors");
    HookVtableEntry(vtable, 24, reinterpret_cast<void*>(&HookCopyDescriptorsSimple), reinterpret_cast<void**>(&g_realCopyDescriptorsSimple), "CopyDescriptorsSimple");
    HookVtableEntry(vtable, 27, reinterpret_cast<void*>(&HookCreateCommittedResource), reinterpret_cast<void**>(&g_realCreateCommittedResource), "CreateCommittedResource");

    ID3D12Device4* device4 = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device4), reinterpret_cast<void**>(&device4))) && device4) {
        void** vtable4 = *reinterpret_cast<void***>(device4);
        CW_SKY_VERBOSE_LOG("[moon-main] ID3D12Device4=%p vtable=%p\n", device4, vtable4);
        HookVtableEntry(vtable4, 53, reinterpret_cast<void*>(&HookCreateCommittedResource1), reinterpret_cast<void**>(&g_realCreateCommittedResource1), "CreateCommittedResource1");
        device4->Release();
    } else {
        CW_SKY_VERBOSE_LOG("[moon-main] ID3D12Device4 unavailable\n");
    }

    ID3D12Device8* device8 = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device8), reinterpret_cast<void**>(&device8))) && device8) {
        void** vtable8 = *reinterpret_cast<void***>(device8);
        CW_SKY_VERBOSE_LOG("[moon-main] ID3D12Device8=%p vtable=%p\n", device8, vtable8);
        HookVtableEntry(vtable8, 69, reinterpret_cast<void*>(&HookCreateCommittedResource2), reinterpret_cast<void**>(&g_realCreateCommittedResource2), "CreateCommittedResource2");
        device8->Release();
    } else {
        CW_SKY_VERBOSE_LOG("[moon-main] ID3D12Device8 unavailable\n");
    }

    ID3D12Device10* device10 = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device10), reinterpret_cast<void**>(&device10))) && device10) {
        void** vtable10 = *reinterpret_cast<void***>(device10);
        CW_SKY_VERBOSE_LOG("[moon-main] ID3D12Device10=%p vtable=%p\n", device10, vtable10);
        HookVtableEntry(vtable10, 76, reinterpret_cast<void*>(&HookCreateCommittedResource3), reinterpret_cast<void**>(&g_realCreateCommittedResource3), "CreateCommittedResource3");
        device10->Release();
    } else {
        CW_SKY_VERBOSE_LOG("[moon-main] ID3D12Device10 unavailable\n");
    }

    SetTextureStatus(g_moonSlot, "Moon texture: device hooks installed");
    Log("[moon-main] D3D12 device hooks ready\n");
}

void InstallDeviceHooks(ID3D12Device* device) {
    __try {
        InstallDeviceHooksBody(device);
    } __except (LogSkyExceptionFilter("InstallDeviceHooks", GetExceptionInformation())) {
    }
}

HRESULT WINAPI HookD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid, void** device) {
    if (g_skyTextureHooksDisabled.load()) {
        return g_realD3D12CreateDevice(adapter, minimumFeatureLevel, riid, device);
    }
    for (TextureSlot* slot : g_textureSlots) {
        LogFingerprintSummary(*slot);
    }
    HRESULT hr = g_realD3D12CreateDevice(adapter, minimumFeatureLevel, riid, device);
    CW_SKY_VERBOSE_LOG("[moon-main] D3D12CreateDevice adapter=%p feature=0x%X hr=0x%08X device=%p\n",
        adapter,
        static_cast<unsigned>(minimumFeatureLevel),
        static_cast<unsigned>(hr),
        device ? *device : nullptr);
    if (SUCCEEDED(hr) && device && *device) {
        InstallDeviceHooks(reinterpret_cast<ID3D12Device*>(*device));
    }
    return hr;
}

DWORD MoonHookBootstrapThreadBody() {
    for (TextureSlot* slot : g_textureSlots) {
        ResolveFingerprintTargets(*slot);
        LogFingerprintSummary(*slot);
    }

    HMODULE d3d12 = nullptr;
    for (int i = 0; i < 300 && !d3d12; ++i) {
        d3d12 = GetModuleHandleA("d3d12.dll");
        if (!d3d12) {
            Sleep(100);
        }
    }

    if (!d3d12) {
        SetTextureStatus(g_moonSlot, "Moon texture: d3d12 not loaded");
        Log("[moon-main] d3d12.dll not loaded; moon proof hook inactive\n");
        return 0;
    }

    void* createDevice = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    g_d3d12CreateDeviceTarget = createDevice;
    CW_SKY_VERBOSE_LOG("[moon-main] d3d12=%p D3D12CreateDevice=%p\n", d3d12, createDevice);
    if (!createDevice) {
        SetTextureStatus(g_moonSlot, "Moon texture: D3D12CreateDevice not found");
        return 0;
    }

    MH_STATUS createHook = MH_CreateHook(createDevice, reinterpret_cast<void*>(&HookD3D12CreateDevice), reinterpret_cast<void**>(&g_realD3D12CreateDevice));
    CW_SKY_VERBOSE_LOG("[moon-main] D3D12CreateDevice hook create=%d\n", static_cast<int>(createHook));
    if (createHook == MH_OK || createHook == MH_ERROR_ALREADY_CREATED) {
        MH_STATUS enableHook = MH_EnableHook(createDevice);
        CW_SKY_VERBOSE_LOG("[moon-main] D3D12CreateDevice hook enable=%d\n", static_cast<int>(enableHook));
        if (enableHook == MH_OK || enableHook == MH_ERROR_ENABLED) {
            g_d3d12CreateDeviceHooked.store(true);
            SetTextureStatus(g_moonSlot, "Moon texture: D3D12 hook installed, waiting for moon");
        }
    }

    return 0;
}

DWORD WINAPI MoonHookBootstrapThread(LPVOID) {
    CW_SKY_VERBOSE_LOG("[moon-main] hook bootstrap thread entered\n");
    __try {
        return MoonHookBootstrapThreadBody();
    } __except (LogSkyExceptionFilter("MoonHookBootstrapThread", GetExceptionInformation())) {
        Log("[moon-main] hook bootstrap thread aborted after exception\n");
        return 0;
    }
}

void StartIntegratedMoonHook() {
    bool expected = false;
    if (!g_hookThreadStarted.compare_exchange_strong(expected, true)) {
        CW_SKY_VERBOSE_LOG("[moon-main] hook bootstrap thread already started\n");
        return;
    }

    g_hookShuttingDown.store(false);
    CW_SKY_VERBOSE_LOG("[moon-main] initializing hook critical section\n");
    InitializeCriticalSection(&g_hookLock);
    g_hookLockReady.store(true);
    StartAnimationFrameLoader();
    CW_SKY_VERBOSE_LOG("[moon-main] creating hook bootstrap thread\n");

    HANDLE thread = CreateThread(nullptr, 0, MoonHookBootstrapThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        SetTextureStatus(g_moonSlot, "Moon texture: hook thread failed");
        Log("[moon-main] failed to start hook bootstrap thread error=%lu\n", GetLastError());
    }
}

void ReleaseMoonHookResources() {
    if (!g_hookLockReady.load()) {
        return;
    }

    g_hookShuttingDown.store(true);
    StopAnimationFrameLoader();
    ID3D12CommandQueue* uploadQueue = g_uploadQueue.exchange(nullptr);
    ID3D12Device* device = g_device.exchange(nullptr);

    EnterCriticalSection(&g_hookLock);
    for (TextureSlot* slot : g_textureSlots) {
        if (!slot) {
            continue;
        }
        slot->selectedPath.clear();
        DeactivateFileResourceLocked(*slot);
        for (CachedTexture& cached : slot->cache) {
            if (cached.resource) {
                cached.resource->Release();
                cached.resource = nullptr;
            }
        }
        slot->cache.clear();
        for (CachedAnimation& cached : slot->animCache) {
            ReleaseAnimationResources(cached);
        }
        slot->animCache.clear();
        ClearTrackedNativeStateLocked(*slot);
    }
    g_commandListHooksInstalled.store(false);
    LeaveCriticalSection(&g_hookLock);

    if (uploadQueue) {
        uploadQueue->Release();
    }
    if (device) {
        device->Release();
    }
}

} // namespace

void SkyTextureOnInitDevice(ID3D12Device* device) {
    if (!device) return;
    CW_SKY_VERBOSE_LOG("[moon-main] ReShade init_device fallback device=%p\n", device);
    InstallDeviceHooks(device);
}

void SkyTextureOnPresent() {
    if (!g_hookLockReady.load() || g_hookShuttingDown.load() || g_skyTextureHooksDisabled.load()) {
        return;
    }

    __try {
        const ULONGLONG now = GetTickCount64();
        std::array<TextureSlot*, sizeof(g_textureSlots) / sizeof(g_textureSlots[0])> loadSlots{};
        size_t loadCount = 0;
        EnterCriticalSection(&g_hookLock);
        for (TextureSlot* slot : g_textureSlots) {
            if (slot->selectedAnimated && !slot->selectedPath.empty() &&
                (!slot->activeAnimated ||
                 !StringEqualsIgnoreCase(slot->activePath, slot->selectedPath))) {
                if (loadCount < loadSlots.size()) {
                    loadSlots[loadCount++] = slot;
                }
            }
        }
        LeaveCriticalSection(&g_hookLock);

        for (size_t i = 0; i < loadCount; ++i) {
            EnsureAnimatedTextureResource(*loadSlots[i], g_device.load());
        }

        EnterCriticalSection(&g_hookLock);
        for (TextureSlot* slotPtr : g_textureSlots) {
            TextureSlot& slot = *slotPtr;
            if (!slot.selectedAnimated || !slot.activeAnimated || slot.activePath.empty()) {
                continue;
            }

            CachedAnimation* cached = FindCachedAnimationLocked(slot, slot.activePath);
            if (!cached || AnimationFrameCount(*cached) == 0 || !TextureSwapReadyLocked(slot)) {
                continue;
            }

            if (slot.animStartTick == 0) {
                slot.animStartTick = now;
            }
            int desiredDirection = slot.animDirection;
            const size_t desiredFrame = AnimationFrameAtElapsedMs(
                *cached,
                slot.animStartFrame,
                slot.animStartDirection,
                now - slot.animStartTick,
                desiredDirection);
            const bool frameChanged = desiredFrame != slot.animFrameIndex;
            slot.animFrameIndex = desiredFrame;
            slot.animDirection = desiredDirection;

            ID3D12Resource* frame = FindAnimationFrameResourceLocked(*cached, slot.animFrameIndex);
            if (!frame) {
                ++slot.animFrameMissCount;
                ++slot.animFrameSkipCount;
                QueueAnimationWindowLocked(slot, *cached, slot.animFrameIndex, slot.animDirection, true);
                LogAnimationDiagnosticsLocked(slot, *cached, slot.animFrameApplied ? "miss-keep-last" : "miss-wait-initial", !slot.animFrameApplied);
                continue;
            }

            if (slot.animFrameApplied && slot.fileResource.load() == frame && !frameChanged) {
                QueueAnimationWindowLocked(slot, *cached, slot.animFrameIndex, slot.animDirection, false);
                LogAnimationDiagnosticsLocked(slot, *cached, nullptr);
                continue;
            }

            slot.fileResource.store(frame);
            const int rewritten = RewriteTrackedDescriptorsLocked(slot, frame);
            if (rewritten > 0) {
                slot.animRewriteCount += static_cast<uint64_t>(rewritten);
            }
            slot.animFrameApplied = rewritten > 0 || slot.animFrameApplied;
            slot.animLastTick = now;
            static std::atomic<uint32_t> s_animAdvanceLogCount{ 0 };
            const uint32_t logIndex = s_animAdvanceLogCount.fetch_add(1);
            if (logIndex < 8) {
                CW_SKY_VERBOSE_LOG("[%s] animation applied frame=%zu rewritten=%d absolute=1\n", slot.logTag, slot.animFrameIndex, rewritten);
            } else if (logIndex == 8) {
                CW_SKY_VERBOSE_LOG("[%s] animation advance log cap reached\n", slot.logTag);
            }
            if (rewritten > 0) {
                SetTextureStatus(slot, "%s: animation live (%zu descriptors)", slot.displayName, slot.bindings.size());
            }
            QueueAnimationWindowLocked(slot, *cached, slot.animFrameIndex, slot.animDirection, false);
            LogAnimationDiagnosticsLocked(slot, *cached, frameChanged ? "advance" : "initial", !slot.animFrameApplied);
        }
        LeaveCriticalSection(&g_hookLock);
    } __except (LogSkyExceptionFilter("SkyTextureOnPresent", GetExceptionInformation())) {
    }
}

bool InitializeSkyTextureOverride(HMODULE module) {
    if (!TextureSwitcherEnabled()) {
        Log("[moon-main] InitializeSkyTextureOverride skipped: TextureSwitcher.Enabled=0\n");
        return false;
    }
    CW_SKY_VERBOSE_LOG("[moon-main] InitializeSkyTextureOverride enter module=%p\n", module);
    StateLockGuard lock;
    CW_SKY_VERBOSE_LOG("[moon-main] InitializeSkyTextureOverride lock acquired\n");
    g_module = module;
    Log("[moon-main] resolving texture fingerprints\n");
    for (TextureSlot* slot : g_textureSlots) {
        ResolveFingerprintTargets(*slot);
    }
    Log("[moon-main] starting D3D12 texture hook\n");
    StartIntegratedMoonHook();
    SetTextureStatus(g_moonSlot, "Moon texture: integrated hook starting");
    CW_SKY_VERBOSE_LOG("[moon-main] integrated moon proof hook starting module=%p\n", module);
    return true;
}

void ShutdownSkyTextureOverride() {
    if (!TextureSwitcherEnabled() && !g_hookLockReady.load()) {
        return;
    }
    StateLockGuard lock;
    if (g_d3d12CreateDeviceTarget) {
        MH_DisableHook(g_d3d12CreateDeviceTarget);
    }
    ReleaseMoonHookResources();
    g_moonSlot.options.clear();
    g_milkywaySlot.options.clear();
    g_moonSlot.optionsScanned = false;
    g_milkywaySlot.optionsScanned = false;
    g_moonSlot.selectedOption = 0;
    g_milkywaySlot.selectedOption = 0;
    SetTextureStatus(g_moonSlot, "Moon texture: stopped");
    SetTextureStatus(g_milkywaySlot, "Milky Way texture: stopped");
}

void MoonTextureReload() {
    if (!TextureSwitcherEnabled()) {
        return;
    }
    std::string selectedName;
    std::string selectedPath;
    bool selectedAnimated = false;
    {
        StateLockGuard lock;
        RefreshMoonTextureListLocked();
        if (const TextureOption* selected = SelectedMoonTextureLocked()) {
            selectedName = selected->name;
            selectedPath = selected->path;
            selectedAnimated = selected->animated;
        }
    }

    if (!selectedPath.empty()) {
        Log("[moon-ui] reload selected %s (%s)\n", selectedName.c_str(), selectedPath.c_str());
        if (g_hookLockReady.load()) {
            EnterCriticalSection(&g_hookLock);
            if (selectedAnimated) {
                InvalidateCachedAnimationLocked(g_moonSlot, selectedPath);
            } else {
                InvalidateCachedTextureLocked(g_moonSlot, selectedPath);
            }
            LeaveCriticalSection(&g_hookLock);
        }
        if (selectedAnimated) {
            ApplyMoonAnimationPath(selectedPath.c_str());
        } else {
            ApplyMoonTexturePath(selectedPath.c_str());
        }
    } else {
        Log("[moon-ui] reload selected Native\n");
        ClearMoonTextureSelection();
    }
}

void MilkywayTextureReload() {
    if (!TextureSwitcherEnabled()) {
        return;
    }
    std::string selectedName;
    std::string selectedPath;
    bool selectedAnimated = false;
    {
        StateLockGuard lock;
        RefreshMilkywayTextureListLocked();
        if (const TextureOption* selected = SelectedMilkywayTextureLocked()) {
            selectedName = selected->name;
            selectedPath = selected->path;
            selectedAnimated = selected->animated;
        }
    }

    if (!selectedPath.empty()) {
        Log("[milkyway-ui] reload selected %s (%s)\n", selectedName.c_str(), selectedPath.c_str());
        if (g_hookLockReady.load()) {
            EnterCriticalSection(&g_hookLock);
            if (selectedAnimated) {
                InvalidateCachedAnimationLocked(g_milkywaySlot, selectedPath);
            } else {
                InvalidateCachedTextureLocked(g_milkywaySlot, selectedPath);
            }
            LeaveCriticalSection(&g_hookLock);
        }
        if (selectedAnimated) {
            ApplyMilkywayAnimationPath(selectedPath.c_str());
        } else {
            ApplyMilkywayTexturePath(selectedPath.c_str());
        }
    } else {
        Log("[milkyway-ui] reload selected Native\n");
        ClearMilkywayTextureSelection();
    }
}

const char* MoonTextureStatus() {
    if (!TextureSwitcherEnabled()) {
        return "Moon texture: disabled by config";
    }
    StateLockGuard lock;
    thread_local char statusCopy[sizeof(g_moonSlot.status)]{};
    CopyTextureStatus(g_moonSlot, statusCopy, sizeof(statusCopy));
    return statusCopy;
}

const char* MilkywayTextureStatus() {
    if (!TextureSwitcherEnabled()) {
        return "Milky Way texture: disabled by config";
    }
    StateLockGuard lock;
    thread_local char statusCopy[sizeof(g_milkywaySlot.status)]{};
    CopyTextureStatus(g_milkywaySlot, statusCopy, sizeof(statusCopy));
    return statusCopy;
}

bool MoonTextureReady() {
    if (!TextureSwitcherEnabled()) {
        return false;
    }
    if (!g_hookLockReady.load()) {
        return false;
    }
    EnterCriticalSection(&g_hookLock);
    const bool ready = MoonSwapReadyLocked();
    LeaveCriticalSection(&g_hookLock);
    return ready;
}

bool MilkywayTextureReady() {
    if (!TextureSwitcherEnabled()) {
        return false;
    }
    if (!g_hookLockReady.load()) {
        return false;
    }
    EnterCriticalSection(&g_hookLock);
    const bool ready = NonProofSwapReadyLocked(g_milkywaySlot);
    LeaveCriticalSection(&g_hookLock);
    return ready;
}

void MoonTextureRefreshList() {
    if (!TextureSwitcherEnabled()) {
        return;
    }
    StateLockGuard lock;
    RefreshMoonTextureListLocked();
    if (g_moonSlot.selectedOption == 0) {
        SetTextureStatus(g_moonSlot, g_moonSlot.options.empty() ? "Moon texture: no options" : "Moon texture: Native");
    }
}

void MilkywayTextureRefreshList() {
    if (!TextureSwitcherEnabled()) {
        return;
    }
    StateLockGuard lock;
    RefreshMilkywayTextureListLocked();
    if (g_milkywaySlot.selectedOption == 0) {
        SetTextureStatus(g_milkywaySlot, g_milkywaySlot.options.empty() ? "Milky Way texture: no options" : "Milky Way texture: Native");
    }
}

int MoonTextureOptionCount() {
    if (!TextureSwitcherEnabled()) {
        return 1;
    }
    StateLockGuard lock;
    if (!g_moonSlot.optionsScanned) {
        RefreshMoonTextureListLocked();
    }
    return static_cast<int>(g_moonSlot.options.size()) + 1;
}

int MilkywayTextureOptionCount() {
    if (!TextureSwitcherEnabled()) {
        return 1;
    }
    StateLockGuard lock;
    if (!g_milkywaySlot.optionsScanned) {
        RefreshMilkywayTextureListLocked();
    }
    return static_cast<int>(g_milkywaySlot.options.size()) + 1;
}

const char* MoonTextureOptionName(int index) {
    if (!TextureSwitcherEnabled()) {
        return index == 0 ? "Native" : "";
    }
    StateLockGuard lock;
    if (!g_moonSlot.optionsScanned) {
        RefreshMoonTextureListLocked();
    }
    if (index == 0) {
        return "Native";
    }
    if (index < 0 || index > static_cast<int>(g_moonSlot.options.size())) {
        return "";
    }
    return g_moonSlot.options[static_cast<size_t>(index - 1)].name.c_str();
}

const char* MilkywayTextureOptionName(int index) {
    if (!TextureSwitcherEnabled()) {
        return index == 0 ? "Native" : "";
    }
    StateLockGuard lock;
    if (!g_milkywaySlot.optionsScanned) {
        RefreshMilkywayTextureListLocked();
    }
    if (index == 0) {
        return "Native";
    }
    if (index < 0 || index > static_cast<int>(g_milkywaySlot.options.size())) {
        return "";
    }
    return g_milkywaySlot.options[static_cast<size_t>(index - 1)].name.c_str();
}

const char* MoonTextureOptionLabel(int index) {
    if (!TextureSwitcherEnabled()) {
        return index == 0 ? "Native" : "";
    }
    StateLockGuard lock;
    if (!g_moonSlot.optionsScanned) {
        RefreshMoonTextureListLocked();
    }
    if (index == 0) {
        return "Native";
    }
    if (index < 0 || index > static_cast<int>(g_moonSlot.options.size())) {
        return "";
    }
    return g_moonSlot.options[static_cast<size_t>(index - 1)].label.c_str();
}

const char* MilkywayTextureOptionLabel(int index) {
    if (!TextureSwitcherEnabled()) {
        return index == 0 ? "Native" : "";
    }
    StateLockGuard lock;
    if (!g_milkywaySlot.optionsScanned) {
        RefreshMilkywayTextureListLocked();
    }
    if (index == 0) {
        return "Native";
    }
    if (index < 0 || index > static_cast<int>(g_milkywaySlot.options.size())) {
        return "";
    }
    return g_milkywaySlot.options[static_cast<size_t>(index - 1)].label.c_str();
}

const char* MoonTextureOptionPack(int index) {
    if (!TextureSwitcherEnabled()) {
        return "";
    }
    StateLockGuard lock;
    if (!g_moonSlot.optionsScanned) {
        RefreshMoonTextureListLocked();
    }
    if (index <= 0 || index > static_cast<int>(g_moonSlot.options.size())) {
        return "";
    }
    return g_moonSlot.options[static_cast<size_t>(index - 1)].pack.c_str();
}

bool MoonTextureOptionIsAnimated(int index) {
    if (!TextureSwitcherEnabled()) {
        return false;
    }
    StateLockGuard lock;
    if (!g_moonSlot.optionsScanned) {
        RefreshMoonTextureListLocked();
    }
    if (index <= 0 || index > static_cast<int>(g_moonSlot.options.size())) {
        return false;
    }
    return g_moonSlot.options[static_cast<size_t>(index - 1)].animated;
}

bool MilkywayTextureOptionIsAnimated(int index) {
    if (!TextureSwitcherEnabled()) {
        return false;
    }
    StateLockGuard lock;
    if (!g_milkywaySlot.optionsScanned) {
        RefreshMilkywayTextureListLocked();
    }
    if (index <= 0 || index > static_cast<int>(g_milkywaySlot.options.size())) {
        return false;
    }
    return g_milkywaySlot.options[static_cast<size_t>(index - 1)].animated;
}

const char* MilkywayTextureOptionPack(int index) {
    if (!TextureSwitcherEnabled()) {
        return "";
    }
    StateLockGuard lock;
    if (!g_milkywaySlot.optionsScanned) {
        RefreshMilkywayTextureListLocked();
    }
    if (index <= 0 || index > static_cast<int>(g_milkywaySlot.options.size())) {
        return "";
    }
    return g_milkywaySlot.options[static_cast<size_t>(index - 1)].pack.c_str();
}

int MoonTextureFindOptionByName(const char* name) {
    if (!TextureSwitcherEnabled()) {
        return 0;
    }
    if (!name || !name[0] || _stricmp(name, "Native") == 0) {
        return 0;
    }

    StateLockGuard lock;
    if (!g_moonSlot.optionsScanned) {
        RefreshMoonTextureListLocked();
    }

    for (size_t i = 0; i < g_moonSlot.options.size(); ++i) {
        if (_stricmp(g_moonSlot.options[i].name.c_str(), name) == 0) {
            return static_cast<int>(i + 1);
        }
    }

    int labelMatch = -1;
    for (size_t i = 0; i < g_moonSlot.options.size(); ++i) {
        if (_stricmp(g_moonSlot.options[i].label.c_str(), name) == 0) {
            if (labelMatch >= 0) {
                return -1;
            }
            labelMatch = static_cast<int>(i + 1);
        }
    }
    if (labelMatch >= 0) {
        return labelMatch;
    }
    return -1;
}

int MilkywayTextureFindOptionByName(const char* name) {
    if (!TextureSwitcherEnabled()) {
        return 0;
    }
    if (!name || !name[0] || _stricmp(name, "Native") == 0) {
        return 0;
    }

    StateLockGuard lock;
    if (!g_milkywaySlot.optionsScanned) {
        RefreshMilkywayTextureListLocked();
    }

    for (size_t i = 0; i < g_milkywaySlot.options.size(); ++i) {
        if (_stricmp(g_milkywaySlot.options[i].name.c_str(), name) == 0) {
            return static_cast<int>(i + 1);
        }
    }

    int labelMatch = -1;
    for (size_t i = 0; i < g_milkywaySlot.options.size(); ++i) {
        if (_stricmp(g_milkywaySlot.options[i].label.c_str(), name) == 0) {
            if (labelMatch >= 0) {
                return -1;
            }
            labelMatch = static_cast<int>(i + 1);
        }
    }
    if (labelMatch >= 0) {
        return labelMatch;
    }
    return -1;
}

int MoonTextureSelectedOption() {
    if (!TextureSwitcherEnabled()) {
        return 0;
    }
    StateLockGuard lock;
    if (!g_moonSlot.optionsScanned) {
        RefreshMoonTextureListLocked();
    }
    if (g_moonSlot.selectedOption < 0 || g_moonSlot.selectedOption > static_cast<int>(g_moonSlot.options.size())) {
        g_moonSlot.selectedOption = 0;
    }
    return g_moonSlot.selectedOption;
}

int MilkywayTextureSelectedOption() {
    if (!TextureSwitcherEnabled()) {
        return 0;
    }
    StateLockGuard lock;
    if (!g_milkywaySlot.optionsScanned) {
        RefreshMilkywayTextureListLocked();
    }
    if (g_milkywaySlot.selectedOption < 0 || g_milkywaySlot.selectedOption > static_cast<int>(g_milkywaySlot.options.size())) {
        g_milkywaySlot.selectedOption = 0;
    }
    return g_milkywaySlot.selectedOption;
}

void MoonTextureSelectOption(int index) {
    if (!TextureSwitcherEnabled()) {
        return;
    }
    std::string selectedName;
    std::string selectedPath;
    bool selectedAnimated = false;
    {
        StateLockGuard lock;
        if (!g_moonSlot.optionsScanned) {
            RefreshMoonTextureListLocked();
        }
        if (index < 0 || index > static_cast<int>(g_moonSlot.options.size())) {
            index = 0;
        }
        if (g_moonSlot.selectedOption == index) {
            return;
        }

        g_moonSlot.selectedOption = index;

        if (const TextureOption* selected = SelectedMoonTextureLocked()) {
            selectedName = selected->name;
            selectedPath = selected->path;
            selectedAnimated = selected->animated;
        }
    }

    if (!selectedPath.empty()) {
        Log("[moon-ui] selected %s (%s)\n", selectedName.c_str(), selectedPath.c_str());
        if (selectedAnimated) {
            ApplyMoonAnimationPath(selectedPath.c_str());
        } else {
            ApplyMoonTexturePath(selectedPath.c_str());
        }
    } else {
        Log("[moon-ui] selected Native\n");
        ClearMoonTextureSelection();
    }
}

void MilkywayTextureSelectOption(int index) {
    if (!TextureSwitcherEnabled()) {
        return;
    }
    std::string selectedName;
    std::string selectedPath;
    bool selectedAnimated = false;
    {
        StateLockGuard lock;
        if (!g_milkywaySlot.optionsScanned) {
            RefreshMilkywayTextureListLocked();
        }
        if (index < 0 || index > static_cast<int>(g_milkywaySlot.options.size())) {
            index = 0;
        }
        if (g_milkywaySlot.selectedOption == index) {
            return;
        }

        g_milkywaySlot.selectedOption = index;

        if (const TextureOption* selected = SelectedMilkywayTextureLocked()) {
            selectedName = selected->name;
            selectedPath = selected->path;
            selectedAnimated = selected->animated;
        }
    }

    if (!selectedPath.empty()) {
        Log("[milkyway-ui] selected %s (%s)\n", selectedName.c_str(), selectedPath.c_str());
        if (selectedAnimated) {
            ApplyMilkywayAnimationPath(selectedPath.c_str());
        } else {
            ApplyMilkywayTexturePath(selectedPath.c_str());
        }
    } else {
        Log("[milkyway-ui] selected Native\n");
        ClearMilkywayTextureSelection();
    }
}

bool MoonTextureSelectByName(const char* name) {
    if (!TextureSwitcherEnabled()) {
        return false;
    }
    const int index = MoonTextureFindOptionByName(name);
    if (index >= 0) {
        MoonTextureSelectOption(index);
        return index > 0;
    }

    const char* missingName = name ? name : "";
    {
        StateLockGuard lock;
        if (g_moonSlot.lastMissingWarning != missingName) {
            g_moonSlot.lastMissingWarning = missingName;
            Log("[W] moon texture preset missing: %s\n", missingName);
        }
    }
    MoonTextureSelectOption(0);
    return false;
}

bool MilkywayTextureSelectByName(const char* name) {
    if (!TextureSwitcherEnabled()) {
        return false;
    }
    const int index = MilkywayTextureFindOptionByName(name);
    if (index >= 0) {
        MilkywayTextureSelectOption(index);
        return index > 0;
    }

    const char* missingName = name ? name : "";
    {
        StateLockGuard lock;
        if (g_milkywaySlot.lastMissingWarning != missingName) {
            g_milkywaySlot.lastMissingWarning = missingName;
            Log("[W] milkyway texture preset missing: %s\n", missingName);
        }
    }
    MilkywayTextureSelectOption(0);
    return false;
}

#endif
