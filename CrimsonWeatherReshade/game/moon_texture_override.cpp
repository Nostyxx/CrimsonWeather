#include "pch.h"

#include "moon_image_loader.h"
#include "moon_texture_override.h"
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
#include <mutex>
#include <string>
#include <vector>

#if defined(CW_WIND_ONLY)

bool InitializeMoonTextureOverride(HMODULE) { return true; }
void ShutdownMoonTextureOverride() {}
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

#else

namespace {

struct MoonTextureOption {
    std::string name;
    std::string label;
    std::string pack;
    std::string path;
};

HMODULE g_module = nullptr;
std::mutex g_mutex;
std::vector<MoonTextureOption> g_moonOptions;
int g_selectedMoonOption = 0;
bool g_moonOptionsScanned = false;
char g_status[192] = "Moon texture: integrated hook waiting";

struct MoonSrvBinding {
    ID3D12Resource* nativeResource = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dest = {};
    D3D12_SHADER_RESOURCE_VIEW_DESC nativeDesc = {};
    bool hasDesc = false;
    bool copied = false;
};

struct CachedMoonTexture {
    std::string path;
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT mips = 0;
    UINT width = 0;
    UINT height = 0;
};

struct MoonFingerprintTarget {
    const char* name = "";
    const char* pattern = "";
    uintptr_t start = 0;
    size_t range = 0;
};

using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
using PFN_CreateCommandQueue = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
using PFN_CreateShaderResourceView = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CopyDescriptors = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CopyDescriptorsSimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
using PFN_CreateCommittedResource = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CreateCommittedResource1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CreateCommittedResource2 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CreateCommittedResource3 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**);

PFN_D3D12CreateDevice g_realD3D12CreateDevice = nullptr;
PFN_CreateCommandQueue g_realCreateCommandQueue = nullptr;
PFN_CreateShaderResourceView g_realCreateShaderResourceView = nullptr;
PFN_CopyDescriptors g_realCopyDescriptors = nullptr;
PFN_CopyDescriptorsSimple g_realCopyDescriptorsSimple = nullptr;
PFN_CreateCommittedResource g_realCreateCommittedResource = nullptr;
PFN_CreateCommittedResource1 g_realCreateCommittedResource1 = nullptr;
PFN_CreateCommittedResource2 g_realCreateCommittedResource2 = nullptr;
PFN_CreateCommittedResource3 g_realCreateCommittedResource3 = nullptr;

CRITICAL_SECTION g_moonHookLock;
std::atomic<bool> g_moonHookLockReady{ false };
std::atomic<bool> g_moonHookThreadStarted{ false };
std::atomic<bool> g_d3d12CreateDeviceHooked{ false };
std::atomic<bool> g_deviceHooksInstalled{ false };
std::atomic<bool> g_nativeMoonLocked{ false };
std::atomic<uint32_t> g_nativeCandidateLogCount{ 0 };
std::atomic<uint32_t> g_nativeRejectLogCount{ 0 };
std::atomic<uint32_t> g_nativeFingerprintRejectCount{ 0 };
std::atomic<uint32_t> g_descriptorCopyLogCount{ 0 };
std::atomic<ID3D12Device*> g_device{ nullptr };
std::atomic<ID3D12CommandQueue*> g_uploadQueue{ nullptr };
std::atomic<ID3D12Resource*> g_fileMoonResource{ nullptr };
void* g_d3d12CreateDeviceTarget = nullptr;
ID3D12Resource* g_nativeMoonResource = nullptr;
std::vector<ID3D12Resource*> g_moonCandidateResources;
std::vector<MoonSrvBinding> g_moonSrvBindings;
std::vector<CachedMoonTexture> g_moonTextureCache;
std::string g_selectedMoonPath;
std::string g_activeMoonPath;
std::string g_lastMissingMoonTextureWarning;
DXGI_FORMAT g_fileMoonFormat = DXGI_FORMAT_UNKNOWN;
UINT g_fileMoonMips = 0;
UINT g_fileMoonWidth = 0;
UINT g_fileMoonHeight = 0;
bool g_moonSawUnormSrv = false;
bool g_moonSawSrgbSrv = false;
uint32_t g_moonSourceSrvCount = 0;
uint32_t g_moonCopiedSrvCount = 0;
int g_lastMoonProofLevel = -99;
bool g_moonFingerprintSummaryLogged = false;
MoonFingerprintTarget g_moonFingerprintTargets[] = {
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
bool g_moonFingerprintResolved = false;

void SetStatus(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, args);
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

std::string MoonRootDirectory() {
    return ModuleDirectory() + "CrimsonWeather\\moon";
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

bool ResolveMoonFingerprintTargets() {
    if (g_moonFingerprintResolved) {
        return true;
    }

    HMODULE exe = GetModuleHandleA(nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
    bool allFound = base != 0;
    for (MoonFingerprintTarget& target : g_moonFingerprintTargets) {
        target.start = ScanExecutableModulePattern(target.pattern);
        if (target.start) {
            Log("[moon-main] fingerprint %s = %p rva=0x%llX range=0x%zX\n",
                target.name,
                reinterpret_cast<void*>(target.start),
                static_cast<unsigned long long>(target.start - base),
                target.range);
        } else {
            allFound = false;
            Log("[W] moon-main fingerprint %s AOB not found\n", target.name);
        }
    }

    g_moonFingerprintResolved = allFound;
    return allFound;
}

void LogMoonFingerprintSummary() {
    if (g_moonFingerprintSummaryLogged) {
        return;
    }

    HMODULE exe = GetModuleHandleA(nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
    int ready = 0;
    for (const MoonFingerprintTarget& target : g_moonFingerprintTargets) {
        if (target.start) {
            ++ready;
        }
    }

    Log("[moon-main] fingerprint summary ready=%d/%zu\n", ready, std::size(g_moonFingerprintTargets));
    for (const MoonFingerprintTarget& target : g_moonFingerprintTargets) {
        if (target.start && base) {
            Log("[moon-main] fingerprint target %-24s rva=0x%llX range=0x%llX\n",
                target.name,
                static_cast<unsigned long long>(target.start - base),
                static_cast<unsigned long long>(target.range));
        } else {
            Log("[W] moon-main fingerprint target %-24s unresolved\n", target.name);
        }
    }
    g_moonFingerprintSummaryLogged = true;
}

bool IsMoonTextureFileName(const std::string& name) {
    return StringEqualsIgnoreCase(name, "moon.dds") || StringEqualsIgnoreCase(name, "moon.png");
}

std::string MoonTextureNameFromPath(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    std::vector<std::string> parts;
    if (!ec) {
        for (const std::filesystem::path& part : relative) {
            parts.push_back(part.string());
        }
    }

    const size_t count = parts.size();
    if (count >= 5 &&
        IsMoonTextureFileName(parts[count - 1]) &&
        StringEqualsIgnoreCase(parts[count - 2], "texture") &&
        StringEqualsIgnoreCase(parts[count - 3], "0002") &&
        StringEqualsIgnoreCase(parts[count - 4], "files")) {
        const size_t packageRootIndex = count - 5;
        if (packageRootIndex > 0) {
            return parts[packageRootIndex - 1];
        }
        return parts[packageRootIndex];
    }

    std::string name = path.parent_path().filename().string();
    if (name.empty() || StringEqualsIgnoreCase(name, "moon")) {
        name = path.filename().string();
    }
    return name;
}

std::string MoonTexturePackFromPath(const std::filesystem::path& root, const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path relative = std::filesystem::relative(path, root, ec);
    std::vector<std::string> parts;
    if (!ec) {
        for (const std::filesystem::path& part : relative) {
            parts.push_back(part.string());
        }
    }

    const size_t count = parts.size();
    if (count >= 5 &&
        IsMoonTextureFileName(parts[count - 1]) &&
        StringEqualsIgnoreCase(parts[count - 2], "texture") &&
        StringEqualsIgnoreCase(parts[count - 3], "0002") &&
        StringEqualsIgnoreCase(parts[count - 4], "files")) {
        return parts[count - 5];
    }

    return path.parent_path().parent_path().filename().string();
}

void RefreshMoonTextureListLocked() {
    const std::string oldPath = (g_selectedMoonOption > 0 && g_selectedMoonOption <= static_cast<int>(g_moonOptions.size()))
        ? g_moonOptions[static_cast<size_t>(g_selectedMoonOption - 1)].path
        : std::string{};

    g_moonOptions.clear();
    const std::filesystem::path root = MoonRootDirectory();
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    if (ec) {
        Log("[W] moon-ui failed to create moon texture folder %s error=%d\n", root.string().c_str(), ec.value());
        ec.clear();
    }
    if (std::filesystem::exists(root, ec)) {
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) {
                continue;
            }

            const std::filesystem::path path = it->path();
            if (!IsMoonTextureFileName(path.filename().string())) {
                continue;
            }

            const std::string label = MoonTextureNameFromPath(root, path);
            const std::string pack = MoonTexturePackFromPath(root, path);
            std::string name = label;
            if (!pack.empty() && !StringEqualsIgnoreCase(pack, "moon")) {
                name += " (" + pack + ")";
            }

            g_moonOptions.push_back({ name, label, (!pack.empty() && !StringEqualsIgnoreCase(pack, "moon")) ? pack : std::string{}, path.string() });
        }
    }

    std::sort(g_moonOptions.begin(), g_moonOptions.end(), [](const MoonTextureOption& a, const MoonTextureOption& b) {
        const int packCmp = _stricmp(a.pack.c_str(), b.pack.c_str());
        if (packCmp != 0) {
            return packCmp < 0;
        }
        return _stricmp(a.label.c_str(), b.label.c_str()) < 0;
    });

    g_selectedMoonOption = 0;
    if (!oldPath.empty()) {
        for (size_t i = 0; i < g_moonOptions.size(); ++i) {
            if (StringEqualsIgnoreCase(g_moonOptions[i].path, oldPath)) {
                g_selectedMoonOption = static_cast<int>(i + 1);
                break;
            }
        }
    }

    g_moonOptionsScanned = true;
    Log("[moon-ui] scanned %zu moon texture option(s) under %s\n", g_moonOptions.size(), root.string().c_str());
}

const MoonTextureOption* SelectedMoonTextureLocked() {
    if (!g_moonOptionsScanned) {
        RefreshMoonTextureListLocked();
    }

    if (g_selectedMoonOption <= 0 || g_selectedMoonOption > static_cast<int>(g_moonOptions.size())) {
        return nullptr;
    }

    return &g_moonOptions[static_cast<size_t>(g_selectedMoonOption - 1)];
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

bool IsBc1Format(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_BC1_TYPELESS ||
           format == DXGI_FORMAT_BC1_UNORM ||
           format == DXGI_FORMAT_BC1_UNORM_SRGB;
}

DXGI_FORMAT SrvFormatForFileMoon(DXGI_FORMAT requested) {
    if (g_fileMoonFormat == DXGI_FORMAT_BC1_UNORM) {
        return requested == DXGI_FORMAT_BC1_UNORM_SRGB ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    }
    if (g_fileMoonFormat == DXGI_FORMAT_BC3_UNORM) {
        return requested == DXGI_FORMAT_BC1_UNORM_SRGB || requested == DXGI_FORMAT_BC3_UNORM_SRGB ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
    }
    if (g_fileMoonFormat == DXGI_FORMAT_R8G8B8A8_UNORM) {
        return requested == DXGI_FORMAT_BC1_UNORM_SRGB || requested == DXGI_FORMAT_BC3_UNORM_SRGB || requested == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            : DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return requested;
}

bool IsNativeMoonDesc(const D3D12_RESOURCE_DESC* desc) {
    return desc &&
           desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc->Width == 512 &&
           desc->Height == 512 &&
           desc->DepthOrArraySize == 1 &&
           desc->MipLevels == 10 &&
           IsBc1Format(desc->Format);
}

bool IsNativeMoonDesc1(const D3D12_RESOURCE_DESC1* desc) {
    return desc &&
           desc->Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
           desc->Width == 512 &&
           desc->Height == 512 &&
           desc->DepthOrArraySize == 1 &&
           desc->MipLevels == 10 &&
           IsBc1Format(desc->Format);
}

bool CurrentStackHasMoonLoaderFingerprint() {
    if (!ResolveMoonFingerprintTargets()) {
        return false;
    }

    void* frames[48]{};
    const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
    int hits = 0;
    bool matchedTargets[std::size(g_moonFingerprintTargets)]{};

    for (USHORT i = 0; i < count; ++i) {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(frames[i]);
        for (size_t targetIndex = 0; targetIndex < std::size(g_moonFingerprintTargets); ++targetIndex) {
            const MoonFingerprintTarget& target = g_moonFingerprintTargets[targetIndex];
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

    return hits >= 3;
}

bool ShouldTrackNativeMoonDesc(const D3D12_RESOURCE_DESC* desc) {
    if (!IsNativeMoonDesc(desc) || g_nativeMoonLocked.load()) {
        return false;
    }
    const bool matched = CurrentStackHasMoonLoaderFingerprint();
    if (!matched) {
        const uint32_t count = g_nativeFingerprintRejectCount.fetch_add(1) + 1;
        if (count == 128) {
            Log("[moon-main] unmatched 512x512 BC1/10mip allocation count=%u; if moon never proves, fingerprint AOB may need a game-patch update\n", count);
        }
    }
    return matched;
}

bool ShouldTrackNativeMoonDesc1(const D3D12_RESOURCE_DESC1* desc) {
    if (!IsNativeMoonDesc1(desc) || g_nativeMoonLocked.load()) {
        return false;
    }
    const bool matched = CurrentStackHasMoonLoaderFingerprint();
    if (!matched) {
        const uint32_t count = g_nativeFingerprintRejectCount.fetch_add(1) + 1;
        if (count == 128) {
            Log("[moon-main] unmatched 512x512 BC1/10mip allocation count=%u; if moon never proves, fingerprint AOB may need a game-patch update\n", count);
        }
    }
    return matched;
}

const char* MoonProofNameLocked() {
    if (!g_nativeMoonResource) {
        return "waiting-native";
    }
    if (!g_moonSawUnormSrv || !g_moonSawSrgbSrv) {
        return "waiting-srv-pair";
    }
    if (g_moonCopiedSrvCount < 2 || g_moonSrvBindings.size() < 4) {
        return "waiting-runtime-copy";
    }
    return g_moonSrvBindings.size() >= 6 ? "proven-strong" : "proven";
}

int MoonProofLevelLocked() {
    if (!g_nativeMoonResource) {
        return 0;
    }
    if (!g_moonSawUnormSrv || !g_moonSawSrgbSrv) {
        return 1;
    }
    if (g_moonCopiedSrvCount < 2 || g_moonSrvBindings.size() < 4) {
        return 2;
    }
    return g_moonSrvBindings.size() >= 6 ? 4 : 3;
}

void UpdateMoonProofStateLocked(const char* reason) {
    const int level = MoonProofLevelLocked();
    if (level == g_lastMoonProofLevel) {
        return;
    }

    g_lastMoonProofLevel = level;
    SetStatus("Moon texture: %s, bindings=%zu source=%u copied=%u",
        MoonProofNameLocked(),
        g_moonSrvBindings.size(),
        g_moonSourceSrvCount,
        g_moonCopiedSrvCount);
    Log("[moon-main] proof %s state=%s level=%d native=%p candidates=%zu bindings=%zu source=%u copied=%u srvPair=%u/%u\n",
        reason,
        MoonProofNameLocked(),
        level,
        g_nativeMoonResource,
        g_moonCandidateResources.size(),
        g_moonSrvBindings.size(),
        g_moonSourceSrvCount,
        g_moonCopiedSrvCount,
        g_moonSawUnormSrv ? 1u : 0u,
        g_moonSawSrgbSrv ? 1u : 0u);
}

bool MoonSwapReadyLocked() {
    return MoonProofLevelLocked() >= 3;
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
    HRESULT hr = g_realCreateCommandQueue
        ? g_realCreateCommandQueue(device, &queueDesc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue))
        : device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    if (FAILED(hr) || !queue) {
        Log("[moon-main] create upload queue failed hr=0x%08X\n", static_cast<unsigned>(hr));
        return nullptr;
    }

    ID3D12CommandQueue* expected = nullptr;
    if (!g_uploadQueue.compare_exchange_strong(expected, queue)) {
        queue->Release();
    }
    return g_uploadQueue.load();
}

CachedMoonTexture* FindCachedMoonTextureLocked(const std::string& path) {
    if (path.empty()) {
        return nullptr;
    }
    for (CachedMoonTexture& cached : g_moonTextureCache) {
        if (StringEqualsIgnoreCase(cached.path, path)) {
            return &cached;
        }
    }
    return nullptr;
}

void ActivateCachedMoonTextureLocked(const CachedMoonTexture& cached) {
    g_activeMoonPath = cached.path;
    g_fileMoonFormat = cached.format;
    g_fileMoonMips = cached.mips;
    g_fileMoonWidth = cached.width;
    g_fileMoonHeight = cached.height;
    g_fileMoonResource.store(cached.resource);
}

void DeactivateFileMoonResourceLocked() {
    g_activeMoonPath.clear();
    g_fileMoonResource.store(nullptr);
    g_fileMoonFormat = DXGI_FORMAT_UNKNOWN;
    g_fileMoonMips = 0;
    g_fileMoonWidth = 0;
    g_fileMoonHeight = 0;
}

void EnforceMoonTextureCacheLimitLocked() {
    constexpr size_t kMoonTextureCacheLimit = 4;
    while (g_moonTextureCache.size() > kMoonTextureCacheLimit) {
        auto evict = g_moonTextureCache.end();
        for (auto it = g_moonTextureCache.begin(); it != g_moonTextureCache.end(); ++it) {
            if (!StringEqualsIgnoreCase(it->path, g_activeMoonPath)) {
                evict = it;
                break;
            }
        }
        if (evict == g_moonTextureCache.end()) {
            return;
        }

        if (evict->resource) {
            evict->resource->Release();
        }
        Log("[moon-main] cache evicted path=%s cached=%zu\n", evict->path.c_str(), g_moonTextureCache.size() - 1);
        g_moonTextureCache.erase(evict);
    }
}

int RewriteTrackedMoonDescriptorsLocked(ID3D12Resource* replacement);

ID3D12Resource* EnsureFileMoonResource(ID3D12Device* device) {
    if (!device) {
        device = g_device.load();
    }
    if (!device) {
        SetStatus("Moon texture: waiting for D3D12 device");
        Log("[moon-main] cannot create selected moon: no D3D12 device\n");
        return nullptr;
    }

    std::string requestedPath;
    EnterCriticalSection(&g_moonHookLock);
    requestedPath = g_selectedMoonPath;
    if (requestedPath.empty()) {
        LeaveCriticalSection(&g_moonHookLock);
        return nullptr;
    }
    if (ID3D12Resource* existing = g_fileMoonResource.load(); existing && StringEqualsIgnoreCase(g_activeMoonPath, requestedPath)) {
        LeaveCriticalSection(&g_moonHookLock);
        return existing;
    }
    if (CachedMoonTexture* cached = FindCachedMoonTextureLocked(requestedPath)) {
        ActivateCachedMoonTextureLocked(*cached);
        SetStatus("Moon texture: cached %ux%u", cached->width, cached->height);
        Log("[moon-main] cache hit path=%s res=%p %ux%u fmt=%s(%u) cached=%zu\n",
            requestedPath.c_str(),
            cached->resource,
            cached->width,
            cached->height,
            FormatName(cached->format),
            static_cast<unsigned>(cached->format),
            g_moonTextureCache.size());
        LeaveCriticalSection(&g_moonHookLock);
        return cached->resource;
    }
    LeaveCriticalSection(&g_moonHookLock);

    MoonImageData image{};
    char loadStatus[128] = {};
    if (!LoadMoonImageFile(requestedPath, image, loadStatus, sizeof(loadStatus))) {
        SetStatus("Moon texture: %s", loadStatus[0] ? loadStatus : "load failed");
        return nullptr;
    }
    const UINT64 requiredBytes = image.sourceRowPitch * image.sourceRows;
    if (image.pixels.size() < requiredBytes) {
        SetStatus("Moon texture: pixel data too small");
        Log("[moon-main] pixel data too small have=%zu need=%llu\n", image.pixels.size(), static_cast<unsigned long long>(requiredBytes));
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
        SetStatus("Moon texture: create texture failed");
        Log("[moon-main] create selected moon texture failed hr=0x%08X\n", static_cast<unsigned>(hr));
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
        SetStatus("Moon texture: create upload failed");
        Log("[moon-main] create upload buffer failed hr=0x%08X\n", static_cast<unsigned>(hr));
        texture->Release();
        return nullptr;
    }

    uint8_t* mapped = nullptr;
    D3D12_RANGE noRead{ 0, 0 };
    hr = upload->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr) || !mapped) {
        SetStatus("Moon texture: upload map failed");
        Log("[moon-main] upload map failed hr=0x%08X\n", static_cast<unsigned>(hr));
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
        SetStatus("Moon texture: upload queue failed");
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
        SetStatus("Moon texture: command list failed");
        Log("[moon-main] command list setup failed hr=0x%08X\n", static_cast<unsigned>(hr));
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
        SetStatus("Moon texture: command close failed");
        Log("[moon-main] command list close failed hr=0x%08X\n", static_cast<unsigned>(hr));
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
        Log("[moon-main] fence create failed hr=0x%08X\n", static_cast<unsigned>(hr));
    }

    if (eventHandle) CloseHandle(eventHandle);
    if (fence) fence->Release();
    list->Release();
    allocator->Release();
    upload->Release();

    EnterCriticalSection(&g_moonHookLock);
    if (!StringEqualsIgnoreCase(g_selectedMoonPath, requestedPath)) {
        Log("[moon-main] discard loaded moon because selection changed path=%s current=%s\n", requestedPath.c_str(), g_selectedMoonPath.c_str());
        texture->Release();
        LeaveCriticalSection(&g_moonHookLock);
        return nullptr;
    }
    if (CachedMoonTexture* cached = FindCachedMoonTextureLocked(requestedPath)) {
        texture->Release();
        ActivateCachedMoonTextureLocked(*cached);
        SetStatus("Moon texture: cached %ux%u", cached->width, cached->height);
        Log("[moon-main] cache won race path=%s res=%p %ux%u fmt=%s(%u) cached=%zu\n",
            requestedPath.c_str(),
            cached->resource,
            cached->width,
            cached->height,
            FormatName(cached->format),
            static_cast<unsigned>(cached->format),
            g_moonTextureCache.size());
        LeaveCriticalSection(&g_moonHookLock);
        return cached->resource;
    }

    g_moonTextureCache.push_back({ requestedPath, texture, image.format, 1, image.width, image.height });
    ActivateCachedMoonTextureLocked(g_moonTextureCache.back());
    EnforceMoonTextureCacheLimitLocked();
    SetStatus("Moon texture: loaded %ux%u", image.width, image.height);
    Log("[moon-main] runtime moon resource ready res=%p %ux%u fmt=%s(%u) uploadSize=%llu cached=%zu\n",
        texture,
        image.width,
        image.height,
        FormatName(image.format),
        static_cast<unsigned>(image.format),
        static_cast<unsigned long long>(uploadSize),
        g_moonTextureCache.size());

    LeaveCriticalSection(&g_moonHookLock);
    return texture;
}

void InvalidateCachedMoonTextureLocked(const std::string& path) {
    for (auto it = g_moonTextureCache.begin(); it != g_moonTextureCache.end(); ++it) {
        if (!StringEqualsIgnoreCase(it->path, path)) {
            continue;
        }

        if (StringEqualsIgnoreCase(g_activeMoonPath, it->path)) {
            RewriteTrackedMoonDescriptorsLocked(nullptr);
            DeactivateFileMoonResourceLocked();
        }
        if (it->resource) {
            it->resource->Release();
        }
        Log("[moon-main] cache invalidated path=%s cached=%zu\n", path.c_str(), g_moonTextureCache.size() - 1);
        g_moonTextureCache.erase(it);
        return;
    }
}

D3D12_SHADER_RESOURCE_VIEW_DESC FileMoonSrvDescFromNative(const D3D12_SHADER_RESOURCE_VIEW_DESC& nativeDesc) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = nativeDesc;
    desc.Format = SrvFormatForFileMoon(nativeDesc.Format);
    if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
        desc.Texture2D.MipLevels = std::max(1u, std::min(desc.Texture2D.MipLevels, g_fileMoonMips));
    }
    return desc;
}

int RewriteTrackedMoonDescriptorsLocked(ID3D12Resource* replacement) {
    ID3D12Device* device = g_device.load();
    if (!device || !g_realCreateShaderResourceView) {
        SetStatus("Moon texture: waiting for D3D12 device");
        return 0;
    }

    int rewritten = 0;
    for (MoonSrvBinding& binding : g_moonSrvBindings) {
        if (!binding.dest.ptr || !binding.hasDesc) {
            continue;
        }

        if (replacement) {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc = FileMoonSrvDescFromNative(binding.nativeDesc);
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

void ClearMoonTextureSelection() {
    if (!g_moonHookLockReady.load()) {
        return;
    }

    EnterCriticalSection(&g_moonHookLock);
    g_selectedMoonPath.clear();
    DeactivateFileMoonResourceLocked();
    const bool ready = MoonSwapReadyLocked();
    const int rewritten = ready ? RewriteTrackedMoonDescriptorsLocked(nullptr) : 0;
    if (rewritten > 0) {
        SetStatus("Moon texture: Native live (%zu descriptors)", g_moonSrvBindings.size());
    } else {
        SetStatus(ready ? "Moon texture: Native live, no descriptors rewritten" : "Moon texture: Native, waiting for proven moon");
    }
    Log("[moon-main] selected Native rewritten=%d trackedBindings=%zu\n", rewritten, g_moonSrvBindings.size());
    LeaveCriticalSection(&g_moonHookLock);
}

void ApplyMoonTexturePath(const char* path) {
    if (!g_moonHookLockReady.load()) {
        return;
    }
    if (!path || !path[0]) {
        ClearMoonTextureSelection();
        return;
    }

    EnterCriticalSection(&g_moonHookLock);
    g_selectedMoonPath = path;
    DeactivateFileMoonResourceLocked();
    SetStatus("Moon texture: loading selected texture");
    Log("[moon-main] selected path=%s trackedBindings=%zu proof=%s\n", g_selectedMoonPath.c_str(), g_moonSrvBindings.size(), MoonProofNameLocked());
    LeaveCriticalSection(&g_moonHookLock);

    ID3D12Resource* fileMoon = EnsureFileMoonResource(g_device.load());

    EnterCriticalSection(&g_moonHookLock);
    const bool ready = MoonSwapReadyLocked();
    const int rewritten = (fileMoon && ready) ? RewriteTrackedMoonDescriptorsLocked(fileMoon) : 0;
    if (fileMoon) {
        if (rewritten > 0) {
            SetStatus("Moon texture: selected moon live (%zu descriptors)", g_moonSrvBindings.size());
        } else {
            SetStatus(ready ? "Moon texture: selected, no descriptors rewritten" : "Moon texture: selected, waiting for proven moon");
        }
    }
    Log("[moon-main] selected apply rewritten=%d ready=%u fileMoon=%p trackedBindings=%zu\n",
        rewritten,
        ready ? 1u : 0u,
        fileMoon,
        g_moonSrvBindings.size());
    LeaveCriticalSection(&g_moonHookLock);
}

void AddMoonCandidateResourceLocked(ID3D12Resource* resource, const char* tag) {
    if (!resource) {
        return;
    }

    if (g_nativeMoonResource && g_nativeMoonResource != resource) {
        const uint32_t rejectIndex = g_nativeRejectLogCount.fetch_add(1);
        if (rejectIndex < 4) {
            D3D12_RESOURCE_DESC desc = resource->GetDesc();
            Log("[moon-main] ignored extra native moon-like resource from %s res=%p %llux%u mips=%u fmt=%s(%u) locked=%p\n",
                tag,
                resource,
                static_cast<unsigned long long>(desc.Width),
                desc.Height,
                desc.MipLevels,
                FormatName(desc.Format),
                static_cast<unsigned>(desc.Format),
                g_nativeMoonResource);
        } else if (rejectIndex == 4) {
            Log("[moon-main] extra native moon-like resource log cap reached\n");
        }
        return;
    }

    if (!g_nativeMoonResource) {
        resource->AddRef();
        g_nativeMoonResource = resource;
        g_nativeMoonLocked.store(true);
        Log("[moon-main] native moon locked res=%p via %s\n", resource, tag);
        UpdateMoonProofStateLocked("native-locked");
    }

    for (ID3D12Resource* item : g_moonCandidateResources) {
        if (item == resource) {
            return;
        }
    }

    resource->AddRef();
    g_moonCandidateResources.push_back(resource);
    const uint32_t index = g_nativeCandidateLogCount.fetch_add(1);
    if (index < 16) {
        D3D12_RESOURCE_DESC desc = resource->GetDesc();
        Log("[moon-main] native moon candidate from %s res=%p %llux%u mips=%u fmt=%s(%u) candidates=%zu\n",
            tag,
            resource,
            static_cast<unsigned long long>(desc.Width),
            desc.Height,
            desc.MipLevels,
            FormatName(desc.Format),
            static_cast<unsigned>(desc.Format),
            g_moonCandidateResources.size());
    }
}

bool IsMoonCandidateResourceLocked(ID3D12Resource* resource) {
    if (!resource) {
        return false;
    }
    for (ID3D12Resource* item : g_moonCandidateResources) {
        if (item == resource) {
            return true;
        }
    }
    return false;
}

MoonSrvBinding* FindMoonBindingLocked(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    if (!handle.ptr) {
        return nullptr;
    }
    for (MoonSrvBinding& binding : g_moonSrvBindings) {
        if (binding.dest.ptr == handle.ptr) {
            return &binding;
        }
    }
    return nullptr;
}

void TrackMoonBindingLocked(ID3D12Resource* nativeResource,
                            const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                            D3D12_CPU_DESCRIPTOR_HANDLE dest,
                            bool copied) {
    if (!nativeResource || !dest.ptr) {
        return;
    }

    MoonSrvBinding* binding = FindMoonBindingLocked(dest);
    constexpr size_t kMaxTrackedMoonBindings = 16;
    if (!binding && g_moonSrvBindings.size() >= kMaxTrackedMoonBindings) {
        static bool loggedCap = false;
        if (!loggedCap) {
            Log("[moon-main] tracked moon descriptor cap reached (%zu)\n", g_moonSrvBindings.size());
            loggedCap = true;
        }
        return;
    }

    const bool isNew = binding == nullptr;
    if (isNew) {
        g_moonSrvBindings.push_back({});
        binding = &g_moonSrvBindings.back();
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

    if (isNew) {
        if (copied) {
            ++g_moonCopiedSrvCount;
        } else {
            ++g_moonSourceSrvCount;
        }
    }

    if (desc && desc->ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D &&
        desc->Texture2D.MostDetailedMip == 0 &&
        desc->Texture2D.MipLevels == 10) {
        if (desc->Format == DXGI_FORMAT_BC1_UNORM) {
            g_moonSawUnormSrv = true;
        } else if (desc->Format == DXGI_FORMAT_BC1_UNORM_SRGB) {
            g_moonSawSrgbSrv = true;
        }
    }

    UpdateMoonProofStateLocked(copied ? "copied-srv" : "source-srv");
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

void MaybeTrackCopiedMoonDescriptor(ID3D12Device* device,
                                    D3D12_CPU_DESCRIPTOR_HANDLE source,
                                    D3D12_CPU_DESCRIPTOR_HANDLE dest,
                                    const char* tag) {
    if (!device || !source.ptr || !dest.ptr || source.ptr == dest.ptr || !g_moonHookLockReady.load()) {
        return;
    }

    ID3D12Resource* fileMoon = nullptr;
    D3D12_SHADER_RESOURCE_VIEW_DESC nativeDesc{};
    bool hasDesc = false;
    EnterCriticalSection(&g_moonHookLock);
    MoonSrvBinding* sourceBinding = FindMoonBindingLocked(source);
    if (sourceBinding && sourceBinding->nativeResource && sourceBinding->hasDesc) {
        ID3D12Resource* nativeResource = sourceBinding->nativeResource;
        nativeResource->AddRef();
        nativeDesc = sourceBinding->nativeDesc;
        hasDesc = true;
        TrackMoonBindingLocked(nativeResource, &nativeDesc, dest, true);
        nativeResource->Release();
        fileMoon = (!g_selectedMoonPath.empty() && MoonSwapReadyLocked()) ? g_fileMoonResource.load() : nullptr;
        if (fileMoon) {
            fileMoon->AddRef();
            const int rewritten = RewriteTrackedMoonDescriptorsLocked(fileMoon);
            SetStatus("Moon texture: selected moon live (%zu descriptors)", g_moonSrvBindings.size());
            Log("[moon-main] auto rewrite after %s rewritten=%d fileMoon=%p\n", tag, rewritten, fileMoon);
        }

        const uint32_t index = g_descriptorCopyLogCount.fetch_add(1);
        if (index < 4) {
            Log("[moon-main] %s copied moon descriptor src=0x%llX dst=0x%llX\n",
                tag,
                static_cast<unsigned long long>(source.ptr),
                static_cast<unsigned long long>(dest.ptr));
        } else if (index == 4) {
            Log("[moon-main] moon descriptor copy log cap reached\n");
        }
    }
    LeaveCriticalSection(&g_moonHookLock);

    if (fileMoon && hasDesc && g_realCreateShaderResourceView) {
        D3D12_SHADER_RESOURCE_VIEW_DESC fileDesc = FileMoonSrvDescFromNative(nativeDesc);
        g_realCreateShaderResourceView(device, fileMoon, &fileDesc, dest);
        fileMoon->Release();
    }
}

HRESULT STDMETHODCALLTYPE HookCreateCommittedResource(ID3D12Device* self,
                                                      const D3D12_HEAP_PROPERTIES* heapProperties,
                                                      D3D12_HEAP_FLAGS heapFlags,
                                                      const D3D12_RESOURCE_DESC* desc,
                                                      D3D12_RESOURCE_STATES initialState,
                                                      const D3D12_CLEAR_VALUE* optimizedClearValue,
                                                      REFIID riidResource,
                                                      void** resource) {
    const bool nativeHit = ShouldTrackNativeMoonDesc(desc);
    HRESULT hr = g_realCreateCommittedResource(self, heapProperties, heapFlags, desc, initialState, optimizedClearValue, riidResource, resource);
    if (nativeHit && SUCCEEDED(hr) && resource && *resource && g_moonHookLockReady.load()) {
        EnterCriticalSection(&g_moonHookLock);
        AddMoonCandidateResourceLocked(reinterpret_cast<ID3D12Resource*>(*resource), "CreateCommittedResource native");
        LeaveCriticalSection(&g_moonHookLock);
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
    const bool nativeHit = ShouldTrackNativeMoonDesc(desc);
    HRESULT hr = g_realCreateCommittedResource1(self, heapProperties, heapFlags, desc, initialState, optimizedClearValue, protectedSession, riidResource, resource);
    if (nativeHit && SUCCEEDED(hr) && resource && *resource && g_moonHookLockReady.load()) {
        EnterCriticalSection(&g_moonHookLock);
        AddMoonCandidateResourceLocked(reinterpret_cast<ID3D12Resource*>(*resource), "CreateCommittedResource1 native");
        LeaveCriticalSection(&g_moonHookLock);
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
    const bool nativeHit = ShouldTrackNativeMoonDesc1(desc);
    HRESULT hr = g_realCreateCommittedResource2(self, heapProperties, heapFlags, desc, initialState, optimizedClearValue, protectedSession, riidResource, resource);
    if (nativeHit && SUCCEEDED(hr) && resource && *resource && g_moonHookLockReady.load()) {
        EnterCriticalSection(&g_moonHookLock);
        AddMoonCandidateResourceLocked(reinterpret_cast<ID3D12Resource*>(*resource), "CreateCommittedResource2 native");
        LeaveCriticalSection(&g_moonHookLock);
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
    const bool nativeHit = ShouldTrackNativeMoonDesc1(desc);
    HRESULT hr = g_realCreateCommittedResource3(self, heapProperties, heapFlags, desc, initialLayout, optimizedClearValue, protectedSession, numCastableFormats, castableFormats, riidResource, resource);
    if (nativeHit && SUCCEEDED(hr) && resource && *resource && g_moonHookLockReady.load()) {
        EnterCriticalSection(&g_moonHookLock);
        AddMoonCandidateResourceLocked(reinterpret_cast<ID3D12Resource*>(*resource), "CreateCommittedResource3 native");
        LeaveCriticalSection(&g_moonHookLock);
    }
    return hr;
}

void STDMETHODCALLTYPE HookCreateShaderResourceView(ID3D12Device* self,
                                                    ID3D12Resource* resource,
                                                    const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                                    D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor) {
    bool hit = false;
    bool active = false;
    D3D12_RESOURCE_DESC resourceDesc{};
    if (resource && g_moonHookLockReady.load()) {
        resourceDesc = resource->GetDesc();
        EnterCriticalSection(&g_moonHookLock);
        hit = IsMoonCandidateResourceLocked(resource);
        if (hit) {
            TrackMoonBindingLocked(resource, desc, destDescriptor, false);
            active = !g_selectedMoonPath.empty() && MoonSwapReadyLocked();
        }
        LeaveCriticalSection(&g_moonHookLock);
    }

    ID3D12Resource* srvResource = resource;
    D3D12_SHADER_RESOURCE_VIEW_DESC fileMoonSrvDesc{};
    const D3D12_SHADER_RESOURCE_VIEW_DESC* srvDesc = desc;
    ID3D12Resource* fileMoonResource = active ? EnsureFileMoonResource(self) : nullptr;
    if (fileMoonResource && desc) {
        fileMoonSrvDesc = FileMoonSrvDescFromNative(*desc);
        srvResource = fileMoonResource;
        srvDesc = &fileMoonSrvDesc;
    }

    g_realCreateShaderResourceView(self, srvResource, srvDesc, destDescriptor);

    if (hit) {
        static std::atomic<uint32_t> s_srvLogCount{ 0 };
        const uint32_t srvLogIndex = s_srvLogCount.fetch_add(1);
        if (srvLogIndex >= 4) {
            if (srvLogIndex == 4) {
                Log("[moon-main] CreateShaderResourceView moon log cap reached\n");
            }
            return;
        }
        Log("[moon-main] CreateShaderResourceView moon res=%p %llux%u mips=%u fmt=%s(%u) srvFmt=%s(%u) desc=0x%llX replaced=%u\n",
            resource,
            static_cast<unsigned long long>(resourceDesc.Width),
            resourceDesc.Height,
            resourceDesc.MipLevels,
            FormatName(resourceDesc.Format),
            static_cast<unsigned>(resourceDesc.Format),
            srvDesc ? FormatName(srvDesc->Format) : "null",
            srvDesc ? static_cast<unsigned>(srvDesc->Format) : 0u,
            static_cast<unsigned long long>(destDescriptor.ptr),
            fileMoonResource ? 1u : 0u);
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

    const UINT increment = self->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT totalDest = TotalDescriptorCount(numDestDescriptorRanges, destDescriptorRangeSizes);
    const UINT totalSrc = TotalDescriptorCount(numSrcDescriptorRanges, srcDescriptorRangeSizes);
    const UINT total = std::min(totalDest, totalSrc);
    for (UINT i = 0; i < total; ++i) {
        const D3D12_CPU_DESCRIPTOR_HANDLE source = DescriptorAt(numSrcDescriptorRanges, srcDescriptorRangeStarts, srcDescriptorRangeSizes, i, increment);
        const D3D12_CPU_DESCRIPTOR_HANDLE dest = DescriptorAt(numDestDescriptorRanges, destDescriptorRangeStarts, destDescriptorRangeSizes, i, increment);
        MaybeTrackCopiedMoonDescriptor(self, source, dest, "CopyDescriptors");
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

    const UINT increment = self->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (UINT i = 0; i < numDescriptors; ++i) {
        D3D12_CPU_DESCRIPTOR_HANDLE source{};
        D3D12_CPU_DESCRIPTOR_HANDLE dest{};
        source.ptr = srcDescriptorRangeStart.ptr + static_cast<SIZE_T>(i) * increment;
        dest.ptr = destDescriptorRangeStart.ptr + static_cast<SIZE_T>(i) * increment;
        MaybeTrackCopiedMoonDescriptor(self, source, dest, "CopyDescriptorsSimple");
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

void InstallDeviceHooks(ID3D12Device* device) {
    if (!device) {
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

    SetStatus("Moon texture: device hooks installed");
}

HRESULT WINAPI HookD3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid, void** device) {
    LogMoonFingerprintSummary();
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

DWORD WINAPI MoonHookBootstrapThread(LPVOID) {
    MH_STATUS initStatus = MH_Initialize();
    Log("[moon-main] MinHook init status=%d\n", static_cast<int>(initStatus));
    ResolveMoonFingerprintTargets();
    LogMoonFingerprintSummary();

    HMODULE d3d12 = nullptr;
    for (int i = 0; i < 300 && !d3d12; ++i) {
        d3d12 = GetModuleHandleA("d3d12.dll");
        if (!d3d12) {
            Sleep(100);
        }
    }

    if (!d3d12) {
        SetStatus("Moon texture: d3d12 not loaded");
        Log("[moon-main] d3d12.dll not loaded; moon proof hook inactive\n");
        return 0;
    }

    void* createDevice = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"));
    g_d3d12CreateDeviceTarget = createDevice;
    Log("[moon-main] d3d12=%p D3D12CreateDevice=%p\n", d3d12, createDevice);
    if (!createDevice) {
        SetStatus("Moon texture: D3D12CreateDevice not found");
        return 0;
    }

    MH_STATUS createHook = MH_CreateHook(createDevice, reinterpret_cast<void*>(&HookD3D12CreateDevice), reinterpret_cast<void**>(&g_realD3D12CreateDevice));
    Log("[moon-main] D3D12CreateDevice hook create=%d\n", static_cast<int>(createHook));
    if (createHook == MH_OK || createHook == MH_ERROR_ALREADY_CREATED) {
        MH_STATUS enableHook = MH_EnableHook(createDevice);
        Log("[moon-main] D3D12CreateDevice hook enable=%d\n", static_cast<int>(enableHook));
        if (enableHook == MH_OK || enableHook == MH_ERROR_ENABLED) {
            g_d3d12CreateDeviceHooked.store(true);
            SetStatus("Moon texture: D3D12 hook installed, waiting for moon");
        }
    }

    return 0;
}

void StartIntegratedMoonHook() {
    bool expected = false;
    if (!g_moonHookThreadStarted.compare_exchange_strong(expected, true)) {
        return;
    }

    InitializeCriticalSection(&g_moonHookLock);
    g_moonHookLockReady.store(true);

    HANDLE thread = CreateThread(nullptr, 0, MoonHookBootstrapThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        SetStatus("Moon texture: hook thread failed");
        Log("[moon-main] failed to start hook bootstrap thread error=%lu\n", GetLastError());
    }
}

void ReleaseMoonHookResources() {
    if (!g_moonHookLockReady.load()) {
        return;
    }

    EnterCriticalSection(&g_moonHookLock);
    g_selectedMoonPath.clear();
    DeactivateFileMoonResourceLocked();
    for (CachedMoonTexture& cached : g_moonTextureCache) {
        if (cached.resource) {
            cached.resource->Release();
            cached.resource = nullptr;
        }
    }
    g_moonTextureCache.clear();

    for (MoonSrvBinding& binding : g_moonSrvBindings) {
        if (binding.nativeResource) {
            binding.nativeResource->Release();
            binding.nativeResource = nullptr;
        }
    }
    g_moonSrvBindings.clear();

    for (ID3D12Resource* resource : g_moonCandidateResources) {
        if (resource) {
            resource->Release();
        }
    }
    g_moonCandidateResources.clear();

    if (g_nativeMoonResource) {
        g_nativeMoonResource->Release();
        g_nativeMoonResource = nullptr;
    }
    LeaveCriticalSection(&g_moonHookLock);

    if (ID3D12CommandQueue* queue = g_uploadQueue.exchange(nullptr)) {
        queue->Release();
    }
    if (ID3D12Device* device = g_device.exchange(nullptr)) {
        device->Release();
    }
}

} // namespace

bool InitializeMoonTextureOverride(HMODULE module) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_module = module;
    ResolveMoonFingerprintTargets();
    StartIntegratedMoonHook();
    SetStatus("Moon texture: integrated hook starting");
    Log("[moon-main] integrated moon proof hook starting module=%p\n", module);
    return true;
}

void ShutdownMoonTextureOverride() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_d3d12CreateDeviceTarget) {
        MH_DisableHook(g_d3d12CreateDeviceTarget);
    }
    ReleaseMoonHookResources();
    g_moonOptions.clear();
    g_moonOptionsScanned = false;
    g_selectedMoonOption = 0;
    SetStatus("Moon texture: stopped");
}

void MoonTextureReload() {
    std::string selectedName;
    std::string selectedPath;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        RefreshMoonTextureListLocked();
        if (const MoonTextureOption* selected = SelectedMoonTextureLocked()) {
            selectedName = selected->name;
            selectedPath = selected->path;
        }
    }

    if (!selectedPath.empty()) {
        Log("[moon-ui] reload selected %s (%s)\n", selectedName.c_str(), selectedPath.c_str());
        if (g_moonHookLockReady.load()) {
            EnterCriticalSection(&g_moonHookLock);
            InvalidateCachedMoonTextureLocked(selectedPath);
            LeaveCriticalSection(&g_moonHookLock);
        }
        ApplyMoonTexturePath(selectedPath.c_str());
    } else {
        Log("[moon-ui] reload selected Native\n");
        ClearMoonTextureSelection();
    }
}

const char* MoonTextureStatus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    thread_local char statusCopy[sizeof(g_status)]{};
    strcpy_s(statusCopy, g_status);
    return statusCopy;
}

bool MoonTextureReady() {
    if (!g_moonHookLockReady.load()) {
        return false;
    }
    EnterCriticalSection(&g_moonHookLock);
    const bool ready = MoonSwapReadyLocked();
    LeaveCriticalSection(&g_moonHookLock);
    return ready;
}

void MoonTextureRefreshList() {
    std::lock_guard<std::mutex> lock(g_mutex);
    RefreshMoonTextureListLocked();
    if (g_selectedMoonOption == 0) {
        SetStatus(g_moonOptions.empty() ? "Moon texture: no options" : "Moon texture: Native");
    }
}

int MoonTextureOptionCount() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_moonOptionsScanned) {
        RefreshMoonTextureListLocked();
    }
    return static_cast<int>(g_moonOptions.size()) + 1;
}

const char* MoonTextureOptionName(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_moonOptionsScanned) {
        RefreshMoonTextureListLocked();
    }
    if (index == 0) {
        return "Native";
    }
    if (index < 0 || index > static_cast<int>(g_moonOptions.size())) {
        return "";
    }
    return g_moonOptions[static_cast<size_t>(index - 1)].name.c_str();
}

const char* MoonTextureOptionLabel(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_moonOptionsScanned) {
        RefreshMoonTextureListLocked();
    }
    if (index == 0) {
        return "Native";
    }
    if (index < 0 || index > static_cast<int>(g_moonOptions.size())) {
        return "";
    }
    return g_moonOptions[static_cast<size_t>(index - 1)].label.c_str();
}

const char* MoonTextureOptionPack(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_moonOptionsScanned) {
        RefreshMoonTextureListLocked();
    }
    if (index <= 0 || index > static_cast<int>(g_moonOptions.size())) {
        return "";
    }
    return g_moonOptions[static_cast<size_t>(index - 1)].pack.c_str();
}

int MoonTextureFindOptionByName(const char* name) {
    if (!name || !name[0] || _stricmp(name, "Native") == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_moonOptionsScanned) {
        RefreshMoonTextureListLocked();
    }

    for (size_t i = 0; i < g_moonOptions.size(); ++i) {
        if (_stricmp(g_moonOptions[i].name.c_str(), name) == 0) {
            return static_cast<int>(i + 1);
        }
    }

    int labelMatch = -1;
    for (size_t i = 0; i < g_moonOptions.size(); ++i) {
        if (_stricmp(g_moonOptions[i].label.c_str(), name) == 0) {
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
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_moonOptionsScanned) {
        RefreshMoonTextureListLocked();
    }
    if (g_selectedMoonOption < 0 || g_selectedMoonOption > static_cast<int>(g_moonOptions.size())) {
        g_selectedMoonOption = 0;
    }
    return g_selectedMoonOption;
}

void MoonTextureSelectOption(int index) {
    std::string selectedName;
    std::string selectedPath;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_moonOptionsScanned) {
            RefreshMoonTextureListLocked();
        }
        if (index < 0 || index > static_cast<int>(g_moonOptions.size())) {
            index = 0;
        }
        if (g_selectedMoonOption == index) {
            return;
        }

        g_selectedMoonOption = index;

        if (const MoonTextureOption* selected = SelectedMoonTextureLocked()) {
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

bool MoonTextureSelectByName(const char* name) {
    const int index = MoonTextureFindOptionByName(name);
    if (index >= 0) {
        MoonTextureSelectOption(index);
        return index > 0;
    }

    const char* missingName = name ? name : "";
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_lastMissingMoonTextureWarning != missingName) {
            g_lastMissingMoonTextureWarning = missingName;
            Log("[W] moon texture preset missing: %s\n", missingName);
        }
    }
    MoonTextureSelectOption(0);
    return false;
}

#endif
