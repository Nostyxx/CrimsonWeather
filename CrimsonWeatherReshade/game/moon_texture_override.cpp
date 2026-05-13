#include "pch.h"

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
const char* MoonTextureOptionPack(int) { return ""; }
int MoonTextureFindOptionByName(const char*) { return 0; }
int MoonTextureSelectedOption() { return 0; }
void MoonTextureSelectOption(int) {}
bool MoonTextureSelectByName(const char*) { return false; }

#else

namespace {

struct MoonTextureOption {
    std::string name;
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
DXGI_FORMAT g_fileMoonFormat = DXGI_FORMAT_UNKNOWN;
UINT g_fileMoonMips = 0;
UINT g_fileMoonWidth = 0;
UINT g_fileMoonHeight = 0;
bool g_moonSawUnormSrv = false;
bool g_moonSawSrgbSrv = false;
uint32_t g_moonSourceSrvCount = 0;
uint32_t g_moonCopiedSrvCount = 0;
int g_lastMoonProofLevel = -99;

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
        StringEqualsIgnoreCase(parts[count - 1], "moon.dds") &&
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
        StringEqualsIgnoreCase(parts[count - 1], "moon.dds") &&
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
    if (std::filesystem::exists(root, ec)) {
        for (std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
            if (ec || !it->is_regular_file(ec)) {
                continue;
            }

            const std::filesystem::path path = it->path();
            if (!StringEqualsIgnoreCase(path.filename().string(), "moon.dds")) {
                continue;
            }

            std::string name = MoonTextureNameFromPath(root, path);
            const std::string pack = MoonTexturePackFromPath(root, path);
            if (!pack.empty() && !StringEqualsIgnoreCase(pack, "moon")) {
                name += " (" + pack + ")";
            }

            g_moonOptions.push_back({ name, (!pack.empty() && !StringEqualsIgnoreCase(pack, "moon")) ? pack : std::string{}, path.string() });
        }
    }

    std::sort(g_moonOptions.begin(), g_moonOptions.end(), [](const MoonTextureOption& a, const MoonTextureOption& b) {
        const int packCmp = _stricmp(a.pack.c_str(), b.pack.c_str());
        if (packCmp != 0) {
            return packCmp < 0;
        }
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
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
    default: return "other";
    }
}

bool IsBc1Format(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_BC1_TYPELESS ||
           format == DXGI_FORMAT_BC1_UNORM ||
           format == DXGI_FORMAT_BC1_UNORM_SRGB;
}

uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool LoadDdsFile(std::vector<uint8_t>& pixels, UINT& width, UINT& height, UINT& mips, DXGI_FORMAT& format) {
    const std::string path = g_selectedMoonPath;
    if (path.empty()) {
        SetStatus("Moon texture: Native");
        return false;
    }

    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "rb");
    if (!f) {
        SetStatus("Moon texture: selected DDS missing");
        Log("[moon-main] selected DDS missing path=%s\n", path.c_str());
        return false;
    }

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 128) {
        fclose(f);
        SetStatus("Moon texture: DDS too small");
        Log("[moon-main] DDS too small size=%ld path=%s\n", size, path.c_str());
        return false;
    }

    std::vector<uint8_t> file(static_cast<size_t>(size));
    if (fread(file.data(), 1, file.size(), f) != file.size()) {
        fclose(f);
        SetStatus("Moon texture: DDS read failed");
        Log("[moon-main] DDS read failed size=%ld path=%s\n", size, path.c_str());
        return false;
    }
    fclose(f);

    if (memcmp(file.data(), "DDS ", 4) != 0 || ReadU32(file.data() + 4) != 124 || ReadU32(file.data() + 76) != 32) {
        SetStatus("Moon texture: invalid DDS header");
        Log("[moon-main] DDS header invalid path=%s\n", path.c_str());
        return false;
    }

    width = ReadU32(file.data() + 16);
    height = ReadU32(file.data() + 12);
    mips = std::max(1u, ReadU32(file.data() + 28));
    const uint32_t pfFlags = ReadU32(file.data() + 80);
    const uint32_t fourcc = ReadU32(file.data() + 84);
    if ((pfFlags & 4) == 0) {
        SetStatus("Moon texture: unsupported DDS format");
        Log("[moon-main] DDS is not FourCC compressed pfFlags=0x%X path=%s\n", pfFlags, path.c_str());
        return false;
    }

    if (fourcc == ReadU32(reinterpret_cast<const uint8_t*>("DXT1"))) {
        format = DXGI_FORMAT_BC1_UNORM;
    } else if (fourcc == ReadU32(reinterpret_cast<const uint8_t*>("DXT5"))) {
        format = DXGI_FORMAT_BC3_UNORM;
    } else {
        char cc[5]{ static_cast<char>(fourcc & 0xFF), static_cast<char>((fourcc >> 8) & 0xFF), static_cast<char>((fourcc >> 16) & 0xFF), static_cast<char>((fourcc >> 24) & 0xFF), 0 };
        SetStatus("Moon texture: unsupported %s", cc);
        Log("[moon-main] DDS unsupported fourcc=%s path=%s\n", cc, path.c_str());
        return false;
    }

    pixels.assign(file.begin() + 128, file.end());
    Log("[moon-main] loaded DDS %s %ux%u mips=%u fmt=%s(%u) bytes=%zu\n",
        path.c_str(),
        width,
        height,
        mips,
        FormatName(format),
        static_cast<unsigned>(format),
        pixels.size());
    return true;
}

DXGI_FORMAT SrvFormatForFileMoon(DXGI_FORMAT requested) {
    if (g_fileMoonFormat == DXGI_FORMAT_BC1_UNORM) {
        return requested == DXGI_FORMAT_BC1_UNORM_SRGB ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    }
    if (g_fileMoonFormat == DXGI_FORMAT_BC3_UNORM) {
        return requested == DXGI_FORMAT_BC1_UNORM_SRGB || requested == DXGI_FORMAT_BC3_UNORM_SRGB ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
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
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) {
        return false;
    }

    void* frames[48]{};
    const USHORT count = CaptureStackBackTrace(0, 48, frames, nullptr);
    const uintptr_t base = reinterpret_cast<uintptr_t>(exe);
    int hits = 0;
    const uintptr_t targets[] = {
        0x105FFC2,
        0x106A0BD,
        0x1069368,
        0x1080E72,
    };

    for (USHORT i = 0; i < count; ++i) {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(frames[i]);
        if (addr < base) {
            continue;
        }

        const uintptr_t rva = addr - base;
        for (uintptr_t target : targets) {
            if (rva >= target && rva <= target + 0x40) {
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
    return CurrentStackHasMoonLoaderFingerprint();
}

bool ShouldTrackNativeMoonDesc1(const D3D12_RESOURCE_DESC1* desc) {
    if (!IsNativeMoonDesc1(desc) || g_nativeMoonLocked.load()) {
        return false;
    }
    return CurrentStackHasMoonLoaderFingerprint();
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

ID3D12CommandQueue* EnsureUploadQueueLocked(ID3D12Device* device) {
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

    EnterCriticalSection(&g_moonHookLock);
    const std::string requestedPath = g_selectedMoonPath;
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

    std::vector<uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    UINT mips = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (!LoadDdsFile(pixels, width, height, mips, format)) {
        LeaveCriticalSection(&g_moonHookLock);
        return nullptr;
    }

    const UINT blockBytes = format == DXGI_FORMAT_BC1_UNORM ? 8u : 16u;
    const UINT sourceRows = std::max(1u, (height + 3u) / 4u);
    const UINT64 sourceRowPitch = static_cast<UINT64>((width + 3u) / 4u) * blockBytes;
    const UINT64 requiredBytes = sourceRowPitch * sourceRows;
    if (pixels.size() < requiredBytes) {
        SetStatus("Moon texture: DDS pixel data too small");
        Log("[moon-main] DDS pixel data too small have=%zu need=%llu\n", pixels.size(), static_cast<unsigned long long>(requiredBytes));
        LeaveCriticalSection(&g_moonHookLock);
        return nullptr;
    }

    D3D12_RESOURCE_DESC texDesc{};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = format;
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
        LeaveCriticalSection(&g_moonHookLock);
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
        LeaveCriticalSection(&g_moonHookLock);
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
        LeaveCriticalSection(&g_moonHookLock);
        return nullptr;
    }

    uint8_t* dst = mapped + layout.Offset;
    const uint8_t* src = pixels.data();
    for (UINT y = 0; y < sourceRows; ++y) {
        memcpy(dst + static_cast<size_t>(y) * layout.Footprint.RowPitch, src + static_cast<size_t>(y) * sourceRowPitch, static_cast<size_t>(sourceRowPitch));
    }
    upload->Unmap(0, nullptr);

    ID3D12CommandQueue* queue = EnsureUploadQueueLocked(device);
    if (!queue) {
        SetStatus("Moon texture: upload queue failed");
        upload->Release();
        texture->Release();
        LeaveCriticalSection(&g_moonHookLock);
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
        LeaveCriticalSection(&g_moonHookLock);
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
        LeaveCriticalSection(&g_moonHookLock);
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

    g_moonTextureCache.push_back({ requestedPath, texture, format, 1, width, height });
    ActivateCachedMoonTextureLocked(g_moonTextureCache.back());
    SetStatus("Moon texture: loaded %ux%u", width, height);
    Log("[moon-main] runtime moon resource ready res=%p %ux%u fmt=%s(%u) uploadSize=%llu cached=%zu\n",
        texture,
        width,
        height,
        FormatName(format),
        static_cast<unsigned>(format),
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
    SetStatus("Moon texture: loading selected DDS");
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

    if (isNew) {
        Log("[moon-main] tracked moon descriptor=0x%llX bindings=%zu copied=%u fmt=%s(%u)\n",
            static_cast<unsigned long long>(dest.ptr),
            g_moonSrvBindings.size(),
            copied ? 1u : 0u,
            desc ? FormatName(desc->Format) : "null",
            desc ? static_cast<unsigned>(desc->Format) : 0u);
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
        if (index < 40) {
            Log("[moon-main] %s copied moon descriptor src=0x%llX dst=0x%llX\n",
                tag,
                static_cast<unsigned long long>(source.ptr),
                static_cast<unsigned long long>(dest.ptr));
        } else if (index == 40) {
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
        Log("[moon-main] CreateCommittedResource native hr=0x%08X res=%p\n", static_cast<unsigned>(hr), *resource);
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
        Log("[moon-main] CreateCommittedResource1 native hr=0x%08X res=%p\n", static_cast<unsigned>(hr), *resource);
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
        Log("[moon-main] CreateCommittedResource2 native hr=0x%08X res=%p\n", static_cast<unsigned>(hr), *resource);
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
        Log("[moon-main] CreateCommittedResource3 native hr=0x%08X res=%p layout=%u castable=%u\n",
            static_cast<unsigned>(hr),
            *resource,
            static_cast<unsigned>(initialLayout),
            numCastableFormats);
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
    std::lock_guard<std::mutex> lock(g_mutex);
    RefreshMoonTextureListLocked();

    if (const MoonTextureOption* selected = SelectedMoonTextureLocked()) {
        Log("[moon-ui] reload selected %s (%s)\n", selected->name.c_str(), selected->path.c_str());
        if (g_moonHookLockReady.load()) {
            EnterCriticalSection(&g_moonHookLock);
            InvalidateCachedMoonTextureLocked(selected->path);
            LeaveCriticalSection(&g_moonHookLock);
        }
        ApplyMoonTexturePath(selected->path.c_str());
    } else {
        Log("[moon-ui] reload selected Native\n");
        ClearMoonTextureSelection();
    }
}

const char* MoonTextureStatus() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
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
        Log("[moon-ui] selected %s (%s)\n", selected->name.c_str(), selected->path.c_str());
        ApplyMoonTexturePath(selected->path.c_str());
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

    Log("[W] moon texture preset missing: %s\n", name ? name : "");
    MoonTextureSelectOption(0);
    return false;
}

#endif
