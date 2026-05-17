#include "pch.h"

#include "sky_image_loader.h"
#include "sky_texture_override.h"
#include "runtime_shared.h"

#include <d3d12.h>
#include <dxgi.h>

#include <filesystem>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if defined(CW_WIND_ONLY)

bool InitializeSkyTextureOverride(HMODULE) { return true; }
void SkyTextureOnInitDevice(ID3D12Device*) {}
void ShutdownSkyTextureOverride() {}
void MoonTextureReload() {}
const char* MoonTextureStatus() { return "unavailable in Wind only"; }
bool MoonTextureReady() { return false; }
void MoonTextureRefreshList() {}
int MoonTextureOptionCount() { return 1; }
const char* MoonTextureOptionName(int) { return "Native"; }
const char* MoonTextureOptionLabel(int) { return "Native"; }
const char* MoonTextureOptionPack(int) { return ""; }
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

    std::string selectedPath;
    std::string activePath;
    std::string lastMissingWarning;
    DXGI_FORMAT fileFormat = DXGI_FORMAT_UNKNOWN;
    UINT fileMips = 0;
    UINT fileWidth = 0;
    UINT fileHeight = 0;
    std::atomic<ID3D12Resource*> fileResource{ nullptr };
    std::vector<CachedTexture> cache;

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
        "8B C4 48 8D 55 A0 48 8B 8D 80 00 00 00 FF 55 88 "
        "EB 61 4C 8B A9 D8 08 00 00 49 8B 45 00 48 8B 80 D8 00 00 00 48 89 45 88",
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

void SetTextureStatus(TextureSlot& slot, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(slot.status, sizeof(slot.status), fmt, args);
    va_end(args);
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
            Log("[%s] fingerprint %s = %p rva=0x%llX range=0x%zX\n",
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
            Log("[%s] fingerprint target %s rva=0x%llX range=0x%llX\n",
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
            if (!IsTextureFileName(slot, path.filename().string())) {
                continue;
            }

            std::string pack;
            std::string label;
            if (!TryTexturePackPath(root, path, slot, pack, label)) {
                continue;
            }

            slot.options.push_back({ label + " (" + pack + ")", label, pack, path.string() });
        }
    }

    std::sort(slot.options.begin(), slot.options.end(), [](const TextureOption& a, const TextureOption& b) {
        const int packCmp = _stricmp(a.pack.c_str(), b.pack.c_str());
        if (packCmp != 0) {
            return packCmp < 0;
        }
        return _stricmp(a.label.c_str(), b.label.c_str()) < 0;
    });

    slot.selectedOption = 0;
    if (!oldPath.empty()) {
        for (size_t i = 0; i < slot.options.size(); ++i) {
            if (StringEqualsIgnoreCase(slot.options[i].path, oldPath)) {
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

DXGI_FORMAT SrvFormatForFileTexture(DXGI_FORMAT fileFormat, DXGI_FORMAT requested) {
    if (fileFormat == DXGI_FORMAT_BC1_UNORM) {
        return requested == DXGI_FORMAT_BC1_UNORM_SRGB ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    }
    if (fileFormat == DXGI_FORMAT_BC3_UNORM) {
        return requested == DXGI_FORMAT_BC1_UNORM_SRGB || requested == DXGI_FORMAT_BC3_UNORM_SRGB ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
    }
    if (fileFormat == DXGI_FORMAT_R8G8B8A8_UNORM) {
        return requested == DXGI_FORMAT_BC1_UNORM_SRGB || requested == DXGI_FORMAT_BC3_UNORM_SRGB || requested == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            : DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return requested;
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
            Log("[%s] stack log cap reached\n", slot.logTag);
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
    Log("[%s] stack %s:%s\n", slot.logTag, tag, used ? line : " <no game frames>");
}

void LogMilkywayStack(const char* tag) {
    LogNativeStack(g_milkywaySlot, tag);
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
    Log("[moon-main] proof %s state=%s level=%d native=%p candidates=%zu bindings=%zu source=%u copied=%u srvPair=%u/%u\n",
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

bool MilkywaySwapReadyLocked() {
    if (g_milkywaySlot.requireContentProof && !g_milkywaySlot.contentProofHit) {
        return false;
    }
    return !g_milkywaySlot.nativeResources.empty() && !g_milkywaySlot.bindings.empty();
}

bool TextureSwapReadyLocked(TextureSlot& slot) {
    return slot.useProof ? MoonSwapReadyLocked() : MilkywaySwapReadyLocked();
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
    slot.fileFormat = cached.format;
    slot.fileMips = cached.mips;
    slot.fileWidth = cached.width;
    slot.fileHeight = cached.height;
    slot.fileResource.store(cached.resource);
}

void DeactivateFileResourceLocked(TextureSlot& slot) {
    slot.activePath.clear();
    slot.fileResource.store(nullptr);
    slot.fileFormat = DXGI_FORMAT_UNKNOWN;
    slot.fileMips = 0;
    slot.fileWidth = 0;
    slot.fileHeight = 0;
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
        Log("[%s] cache evicted path=%s cached=%zu\n", slot.logTag, evict->path.c_str(), slot.cache.size() - 1);
        slot.cache.erase(evict);
    }
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
        Log("[%s] cache hit path=%s res=%p %ux%u fmt=%s(%u) cached=%zu\n",
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
    if (!LoadSkyImageFile(requestedPath, image, loadStatus, sizeof(loadStatus))) {
        SetTextureStatus(slot, "%s: %s", slot.displayName, loadStatus[0] ? loadStatus : "load failed");
        return nullptr;
    }
    const UINT64 requiredBytes = image.sourceRowPitch * image.sourceRows;
    if (image.pixels.size() < requiredBytes) {
        SetTextureStatus(slot, "%s: pixel data too small", slot.displayName);
        Log("[%s] pixel data too small have=%zu need=%llu\n", slot.logTag, image.pixels.size(), static_cast<unsigned long long>(requiredBytes));
        return nullptr;
    }

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = image.width;
    texDesc.Height = image.height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
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
        Log("[%s] create selected texture failed hr=0x%08X\n", slot.logTag, static_cast<unsigned>(hr));
        return nullptr;
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    UINT64 uploadSize = 0;
    device->GetCopyableFootprints(&texDesc, 0, 1, 0, &layout, &numRows, &rowSize, &uploadSize);

    D3D12_RESOURCE_DESC uploadDesc{};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadSize;
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
        Log("[%s] create upload buffer failed hr=0x%08X\n", slot.logTag, static_cast<unsigned>(hr));
        texture->Release();
        return nullptr;
    }

    uint8_t* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    hr = upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr) || !mapped) {
        SetTextureStatus(slot, "%s: upload map failed", slot.displayName);
        Log("[%s] upload map failed hr=0x%08X\n", slot.logTag, static_cast<unsigned>(hr));
        upload->Release();
        texture->Release();
        return nullptr;
    }

    uint8_t* dst = mapped + layout.Offset;
    const uint8_t* src = image.pixels.data();
    for (UINT y = 0; y < image.sourceRows; ++y) {
        memcpy(dst + static_cast<size_t>(y) * layout.Footprint.RowPitch, src + static_cast<size_t>(y) * image.sourceRowPitch, static_cast<size_t>(image.sourceRowPitch));
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
        Log("[%s] command list setup failed hr=0x%08X\n", slot.logTag, static_cast<unsigned>(hr));
        if (list) list->Release();
        if (allocator) allocator->Release();
        upload->Release();
        texture->Release();
        return nullptr;
    }

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = texture;
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = upload;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = layout;
    list->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

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
        Log("[%s] command list close failed hr=0x%08X\n", slot.logTag, static_cast<unsigned>(hr));
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
        Log("[%s] fence create failed hr=0x%08X\n", slot.logTag, static_cast<unsigned>(hr));
    }

    if (eventHandle) CloseHandle(eventHandle);
    if (fence) fence->Release();
    list->Release();
    allocator->Release();
    upload->Release();

    EnterCriticalSection(&g_hookLock);
    if (!StringEqualsIgnoreCase(slot.selectedPath, requestedPath)) {
        Log("[%s] discard loaded texture because selection changed path=%s current=%s\n", slot.logTag, requestedPath.c_str(), slot.selectedPath.c_str());
        texture->Release();
        LeaveCriticalSection(&g_hookLock);
        return nullptr;
    }
    if (CachedTexture* cached = FindCachedTextureLocked(slot, requestedPath)) {
        texture->Release();
        ActivateCachedTextureLocked(slot, *cached);
        SetTextureStatus(slot, "%s: cached %ux%u", slot.displayName, cached->width, cached->height);
        Log("[%s] cache won race path=%s res=%p %ux%u fmt=%s(%u) cached=%zu\n",
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

    slot.cache.push_back({ requestedPath, texture, image.format, 1, image.width, image.height });
    ActivateCachedTextureLocked(slot, slot.cache.back());
    EnforceCacheLimitLocked(slot);
    SetTextureStatus(slot, "%s: loaded %ux%u", slot.displayName, image.width, image.height);
    Log("[%s] runtime texture ready res=%p %ux%u fmt=%s(%u) uploadSize=%llu cached=%zu\n",
        slot.logTag,
        texture,
        image.width,
        image.height,
        FormatName(image.format),
        static_cast<unsigned>(image.format),
        static_cast<unsigned long long>(uploadSize),
        slot.cache.size());

    LeaveCriticalSection(&g_hookLock);
    return texture;
}

D3D12_SHADER_RESOURCE_VIEW_DESC FileSrvDescFromNative(TextureSlot& slot, const D3D12_SHADER_RESOURCE_VIEW_DESC& nativeDesc) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = nativeDesc;
    desc.Format = SrvFormatForFileTexture(slot.fileFormat, nativeDesc.Format);
    if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
        desc.Texture2D.MipLevels = std::max(1u, std::min(desc.Texture2D.MipLevels, slot.fileMips));
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

void ClearTextureSelection(TextureSlot& slot) {
    if (!g_hookLockReady.load()) {
        return;
    }

    EnterCriticalSection(&g_hookLock);
    slot.selectedPath.clear();
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

void ClearTrackedNativeStateLocked(TextureSlot& slot) {
    for (SrvBinding& binding : slot.bindings) {
        if (binding.nativeResource) {
            binding.nativeResource->Release();
        }
    }
    slot.bindings.clear();

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

    const bool replacingCandidate = slot.primaryNativeResource && slot.primaryNativeResource != resource;
    if (replacingCandidate) {
        Log("[%s] content proof replacing previous candidate old=%p new=%p\n",
            slot.logTag,
            slot.primaryNativeResource,
            resource);
        ClearTrackedNativeStateLocked(slot);
    }

    if (replacingCandidate || !slot.contentProofHit) {
        slot.contentProofHit = true;
        Log("[%s] content proof matched hash=0x%llX expected=0x%llX rowPitch=%u via %s res=%p\n",
            slot.logTag,
            static_cast<unsigned long long>(hash),
            static_cast<unsigned long long>(slot.expectedTopMipHash),
            rowPitch,
            tag ? tag : "native-copy",
            resource);
    }

    TrackNativeResourceLocked(slot, resource, tag ? tag : "content-proof");
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
            Log("[%s] ignored extra native texture-like resource from %s res=%p %llux%u mips=%u fmt=%s(%u) locked=%p\n",
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
            Log("[%s] extra native texture-like resource log cap reached\n", slot.logTag);
        }
        return;
    }

    if (slot.lockFirstNative && !slot.primaryNativeResource) {
        resource->AddRef();
        slot.primaryNativeResource = resource;
        slot.nativeLocked.store(true);
        Log("[%s] native texture locked res=%p via %s\n", slot.logTag, resource, tag);
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
        Log("[%s] native resource from %s res=%p %llux%u mips=%u fmt=%s(%u) candidates=%zu\n",
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
            Log("[%s] tracked descriptor cap reached (%zu)\n", slot.logTag, slot.bindings.size());
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
        Log("[%s] descriptor %s desc=0x%llX copied=%u fmt=%s(%u) bindings=%zu\n",
            slot.logTag,
            tag,
            static_cast<unsigned long long>(dest.ptr),
            copied ? 1u : 0u,
            desc ? FormatName(desc->Format) : "null",
            desc ? static_cast<unsigned>(desc->Format) : 0u,
            slot.bindings.size());
    } else if (index == 12) {
        Log("[%s] descriptor log cap reached\n", slot.logTag);
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
                Log("[%s] auto rewrite after %s rewritten=%d file=%p\n", slot.logTag, tag, rewritten, fileTexture);
            }
        }

        const uint32_t index = slot.descriptorCopyLogCount.fetch_add(1);
        if (index < 4) {
            Log("[%s] %s copied descriptor src=0x%llX dst=0x%llX\n",
                slot.logTag,
                tag,
                static_cast<unsigned long long>(source.ptr),
                static_cast<unsigned long long>(dest.ptr));
        } else if (index == 4) {
            Log("[%s] descriptor copy log cap reached\n", slot.logTag);
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
            Log("[%s] content proof skipped non-upload source=%p dim=%u width=%llu\n",
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
            Log("[%s] content proof native map failed hr=0x%08X source=%p dest=%p offset=%llu rowPitch=%u\n",
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
        if (IsNativeResourceLocked(slot, dst->pResource)) {
            const uint32_t count = slot.contentRejectLogCount.fetch_add(1);
            if (count < 6) {
                Log("[%s] content proof observed hash=0x%llX dest=%p offset=%llu rowPitch=%u (not enforced yet)\n",
                    slot.logTag,
                    static_cast<unsigned long long>(hash),
                    dst->pResource,
                    static_cast<unsigned long long>(src->PlacedFootprint.Offset),
                    src->PlacedFootprint.Footprint.RowPitch);
            }
            EnterCriticalSection(&g_hookLock);
            PromoteContentProvenTextureLocked(slot, dst->pResource, "CopyTextureRegionObserved", hash, src->PlacedFootprint.Footprint.RowPitch);
            LeaveCriticalSection(&g_hookLock);
        }
        return;
    }

    if (hash != slot.expectedTopMipHash) {
        const uint32_t count = slot.contentRejectLogCount.fetch_add(1);
        if (count < 12) {
            Log("[%s] content proof rejected hash=0x%llX expected=0x%llX dest=%p offset=%llu rowPitch=%u\n",
                slot.logTag,
                static_cast<unsigned long long>(hash),
                static_cast<unsigned long long>(slot.expectedTopMipHash),
                dst->pResource,
                static_cast<unsigned long long>(src->PlacedFootprint.Offset),
                src->PlacedFootprint.Footprint.RowPitch);
        } else if (count == 12) {
            Log("[%s] content proof reject log cap reached\n", slot.logTag);
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
    Log("[moon-main] installing ID3D12GraphicsCommandList hooks list=%p vtable=%p\n", list, vtable);
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
        LeaveCriticalSection(&g_hookLock);
    }

    ID3D12Resource* srvResource = resource;
    D3D12_SHADER_RESOURCE_VIEW_DESC fileSrvDesc{};
    const D3D12_SHADER_RESOURCE_VIEW_DESC* srvDesc = desc;
    ID3D12Resource* fileResource = nullptr;
    if (hitSlot && active) {
        __try {
            fileResource = EnsureFileResource(*hitSlot, self);
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

    Log("[moon-main] hooked %s index=%d target=%p\n", name, index, vtable[index]);
    return true;
}

void InstallDeviceHooksBody(ID3D12Device* device) {
    if (g_device.load()) {
        Log("[moon-main] InstallDeviceHooks skipped, device already captured\n");
        return;
    }

    bool expected = false;
    if (!g_deviceHooksInstalled.compare_exchange_strong(expected, true)) {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    Log("[moon-main] installing ID3D12Device hooks device=%p vtable=%p\n", device, vtable);

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
        Log("[moon-main] ID3D12Device4=%p vtable=%p\n", device4, vtable4);
        HookVtableEntry(vtable4, 53, reinterpret_cast<void*>(&HookCreateCommittedResource1), reinterpret_cast<void**>(&g_realCreateCommittedResource1), "CreateCommittedResource1");
        device4->Release();
    } else {
        Log("[moon-main] ID3D12Device4 unavailable\n");
    }

    ID3D12Device8* device8 = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device8), reinterpret_cast<void**>(&device8))) && device8) {
        void** vtable8 = *reinterpret_cast<void***>(device8);
        Log("[moon-main] ID3D12Device8=%p vtable=%p\n", device8, vtable8);
        HookVtableEntry(vtable8, 69, reinterpret_cast<void*>(&HookCreateCommittedResource2), reinterpret_cast<void**>(&g_realCreateCommittedResource2), "CreateCommittedResource2");
        device8->Release();
    } else {
        Log("[moon-main] ID3D12Device8 unavailable\n");
    }

    ID3D12Device10* device10 = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(ID3D12Device10), reinterpret_cast<void**>(&device10))) && device10) {
        void** vtable10 = *reinterpret_cast<void***>(device10);
        Log("[moon-main] ID3D12Device10=%p vtable=%p\n", device10, vtable10);
        HookVtableEntry(vtable10, 76, reinterpret_cast<void*>(&HookCreateCommittedResource3), reinterpret_cast<void**>(&g_realCreateCommittedResource3), "CreateCommittedResource3");
        device10->Release();
    } else {
        Log("[moon-main] ID3D12Device10 unavailable\n");
    }

    SetTextureStatus(g_moonSlot, "Moon texture: device hooks installed");
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
    Log("[moon-main] D3D12CreateDevice adapter=%p feature=0x%X hr=0x%08X device=%p\n",
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
    Log("[moon-main] d3d12=%p D3D12CreateDevice=%p\n", d3d12, createDevice);
    if (!createDevice) {
        SetTextureStatus(g_moonSlot, "Moon texture: D3D12CreateDevice not found");
        return 0;
    }

    MH_STATUS createHook = MH_CreateHook(createDevice, reinterpret_cast<void*>(&HookD3D12CreateDevice), reinterpret_cast<void**>(&g_realD3D12CreateDevice));
    Log("[moon-main] D3D12CreateDevice hook create=%d\n", static_cast<int>(createHook));
    if (createHook == MH_OK || createHook == MH_ERROR_ALREADY_CREATED) {
        MH_STATUS enableHook = MH_EnableHook(createDevice);
        Log("[moon-main] D3D12CreateDevice hook enable=%d\n", static_cast<int>(enableHook));
        if (enableHook == MH_OK || enableHook == MH_ERROR_ENABLED) {
            g_d3d12CreateDeviceHooked.store(true);
            SetTextureStatus(g_moonSlot, "Moon texture: D3D12 hook installed, waiting for moon");
        }
    }

    return 0;
}

DWORD WINAPI MoonHookBootstrapThread(LPVOID) {
    Log("[moon-main] hook bootstrap thread entered\n");
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
        Log("[moon-main] hook bootstrap thread already started\n");
        return;
    }

    g_hookShuttingDown.store(false);
    Log("[moon-main] initializing hook critical section\n");
    InitializeCriticalSection(&g_hookLock);
    g_hookLockReady.store(true);
    Log("[moon-main] creating hook bootstrap thread\n");

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
    ID3D12CommandQueue* uploadQueue = g_uploadQueue.exchange(nullptr);
    ID3D12Device* device = g_device.exchange(nullptr);

    EnterCriticalSection(&g_hookLock);
    g_moonSlot.selectedPath.clear();
    g_milkywaySlot.selectedPath.clear();
    DeactivateFileResourceLocked(g_moonSlot);
    DeactivateFileResourceLocked(g_milkywaySlot);
    for (CachedTexture& cached : g_moonSlot.cache) {
        if (cached.resource) {
            cached.resource->Release();
            cached.resource = nullptr;
        }
    }
    g_moonSlot.cache.clear();
    for (CachedTexture& cached : g_milkywaySlot.cache) {
        if (cached.resource) {
            cached.resource->Release();
            cached.resource = nullptr;
        }
    }
    g_milkywaySlot.cache.clear();

    for (SrvBinding& binding : g_moonSlot.bindings) {
        if (binding.nativeResource) {
            binding.nativeResource->Release();
            binding.nativeResource = nullptr;
        }
    }
    g_moonSlot.bindings.clear();
    for (SrvBinding& binding : g_milkywaySlot.bindings) {
        if (binding.nativeResource) {
            binding.nativeResource->Release();
            binding.nativeResource = nullptr;
        }
    }
    g_milkywaySlot.bindings.clear();

    for (ID3D12Resource* resource : g_moonSlot.nativeResources) {
        if (resource) {
            resource->Release();
        }
    }
    g_moonSlot.nativeResources.clear();
    for (ID3D12Resource* resource : g_milkywaySlot.nativeResources) {
        if (resource) {
            resource->Release();
        }
    }
    g_milkywaySlot.nativeResources.clear();

    if (g_moonSlot.primaryNativeResource) {
        g_moonSlot.primaryNativeResource->Release();
        g_moonSlot.primaryNativeResource = nullptr;
    }
    g_moonSlot.nativeLocked.store(false);
    g_moonSlot.contentProofHit = false;
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
    Log("[moon-main] ReShade init_device fallback device=%p\n", device);
    InstallDeviceHooks(device);
}

bool InitializeSkyTextureOverride(HMODULE module) {
    Log("[moon-main] InitializeSkyTextureOverride enter module=%p\n", module);
    StateLockGuard lock;
    Log("[moon-main] InitializeSkyTextureOverride lock acquired\n");
    g_module = module;
    Log("[moon-main] resolving texture fingerprints\n");
    for (TextureSlot* slot : g_textureSlots) {
        ResolveFingerprintTargets(*slot);
    }
    Log("[moon-main] starting integrated D3D12 texture hook\n");
    StartIntegratedMoonHook();
    SetTextureStatus(g_moonSlot, "Moon texture: integrated hook starting");
    Log("[moon-main] integrated moon proof hook starting module=%p\n", module);
    return true;
}

void ShutdownSkyTextureOverride() {
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
    std::string selectedName;
    std::string selectedPath;
    {
        StateLockGuard lock;
        RefreshMoonTextureListLocked();
        if (const TextureOption* selected = SelectedMoonTextureLocked()) {
            selectedName = selected->name;
            selectedPath = selected->path;
        }
    }

    if (!selectedPath.empty()) {
        Log("[moon-ui] reload selected %s (%s)\n", selectedName.c_str(), selectedPath.c_str());
        if (g_hookLockReady.load()) {
            EnterCriticalSection(&g_hookLock);
            InvalidateCachedTextureLocked(g_moonSlot, selectedPath);
            LeaveCriticalSection(&g_hookLock);
        }
        ApplyMoonTexturePath(selectedPath.c_str());
    } else {
        Log("[moon-ui] reload selected Native\n");
        ClearMoonTextureSelection();
    }
}

void MilkywayTextureReload() {
    std::string selectedName;
    std::string selectedPath;
    {
        StateLockGuard lock;
        RefreshMilkywayTextureListLocked();
        if (const TextureOption* selected = SelectedMilkywayTextureLocked()) {
            selectedName = selected->name;
            selectedPath = selected->path;
        }
    }

    if (!selectedPath.empty()) {
        Log("[milkyway-ui] reload selected %s (%s)\n", selectedName.c_str(), selectedPath.c_str());
        if (g_hookLockReady.load()) {
            EnterCriticalSection(&g_hookLock);
            InvalidateCachedTextureLocked(g_milkywaySlot, selectedPath);
            LeaveCriticalSection(&g_hookLock);
        }
        ApplyMilkywayTexturePath(selectedPath.c_str());
    } else {
        Log("[milkyway-ui] reload selected Native\n");
        ClearMilkywayTextureSelection();
    }
}

const char* MoonTextureStatus() {
    StateLockGuard lock;
    thread_local char statusCopy[sizeof(g_moonSlot.status)]{};
    strcpy_s(statusCopy, g_moonSlot.status);
    return statusCopy;
}

const char* MilkywayTextureStatus() {
    StateLockGuard lock;
    thread_local char statusCopy[sizeof(g_milkywaySlot.status)]{};
    strcpy_s(statusCopy, g_milkywaySlot.status);
    return statusCopy;
}

bool MoonTextureReady() {
    if (!g_hookLockReady.load()) {
        return false;
    }
    EnterCriticalSection(&g_hookLock);
    const bool ready = MoonSwapReadyLocked();
    LeaveCriticalSection(&g_hookLock);
    return ready;
}

bool MilkywayTextureReady() {
    if (!g_hookLockReady.load()) {
        return false;
    }
    EnterCriticalSection(&g_hookLock);
    const bool ready = MilkywaySwapReadyLocked();
    LeaveCriticalSection(&g_hookLock);
    return ready;
}

void MoonTextureRefreshList() {
    StateLockGuard lock;
    RefreshMoonTextureListLocked();
    if (g_moonSlot.selectedOption == 0) {
        SetTextureStatus(g_moonSlot, g_moonSlot.options.empty() ? "Moon texture: no options" : "Moon texture: Native");
    }
}

void MilkywayTextureRefreshList() {
    StateLockGuard lock;
    RefreshMilkywayTextureListLocked();
    if (g_milkywaySlot.selectedOption == 0) {
        SetTextureStatus(g_milkywaySlot, g_milkywaySlot.options.empty() ? "Milky Way texture: no options" : "Milky Way texture: Native");
    }
}

int MoonTextureOptionCount() {
    StateLockGuard lock;
    if (!g_moonSlot.optionsScanned) {
        RefreshMoonTextureListLocked();
    }
    return static_cast<int>(g_moonSlot.options.size()) + 1;
}

int MilkywayTextureOptionCount() {
    StateLockGuard lock;
    if (!g_milkywaySlot.optionsScanned) {
        RefreshMilkywayTextureListLocked();
    }
    return static_cast<int>(g_milkywaySlot.options.size()) + 1;
}

const char* MoonTextureOptionName(int index) {
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
    StateLockGuard lock;
    if (!g_moonSlot.optionsScanned) {
        RefreshMoonTextureListLocked();
    }
    if (index <= 0 || index > static_cast<int>(g_moonSlot.options.size())) {
        return "";
    }
    return g_moonSlot.options[static_cast<size_t>(index - 1)].pack.c_str();
}

const char* MilkywayTextureOptionPack(int index) {
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
    std::string selectedName;
    std::string selectedPath;
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
        }
    }

    if (!selectedPath.empty()) {
        Log("[moon-ui] selected %s (%s)\n", selectedName.c_str(), selectedPath.c_str());
        ApplyMoonTexturePath(selectedPath.c_str());
    } else {
        Log("[moon-ui] selected Native\n");
        ClearMoonTextureSelection();
    }
}

void MilkywayTextureSelectOption(int index) {
    std::string selectedName;
    std::string selectedPath;
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
        }
    }

    if (!selectedPath.empty()) {
        Log("[milkyway-ui] selected %s (%s)\n", selectedName.c_str(), selectedPath.c_str());
        ApplyMilkywayTexturePath(selectedPath.c_str());
    } else {
        Log("[milkyway-ui] selected Native\n");
        ClearMilkywayTextureSelection();
    }
}

bool MoonTextureSelectByName(const char* name) {
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
