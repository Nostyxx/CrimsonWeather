#include "pch.h"

#include "moon_image_loader.h"
#include "runtime_shared.h"

#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool EndsWithNoCase(const std::string& text, const char* suffix) {
    const size_t textLen = text.size();
    const size_t suffixLen = strlen(suffix);
    if (suffixLen > textLen) {
        return false;
    }
    return _stricmp(text.c_str() + textLen - suffixLen, suffix) == 0;
}

void SetLoadStatus(char* status, size_t statusSize, const char* text) {
    if (!status || statusSize == 0) {
        return;
    }
    snprintf(status, statusSize, "%s", text ? text : "");
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wide.data(), needed);
    if (!wide.empty() && wide.back() == L'\0') {
        wide.pop_back();
    }
    return wide;
}

const char* FormatName(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_BC1_UNORM: return "BC1_UNORM";
    case DXGI_FORMAT_BC3_UNORM: return "BC3_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    default: return "other";
    }
}

bool LoadDdsFile(const std::string& path, MoonImageData& outImage, char* status, size_t statusSize) {
    FILE* f = nullptr;
    fopen_s(&f, path.c_str(), "rb");
    if (!f) {
        SetLoadStatus(status, statusSize, "selected DDS missing");
        Log("[moon-main] selected DDS missing path=%s\n", path.c_str());
        return false;
    }

    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 128) {
        fclose(f);
        SetLoadStatus(status, statusSize, "DDS too small");
        Log("[moon-main] DDS too small size=%ld path=%s\n", size, path.c_str());
        return false;
    }

    std::vector<uint8_t> file(static_cast<size_t>(size));
    if (fread(file.data(), 1, file.size(), f) != file.size()) {
        fclose(f);
        SetLoadStatus(status, statusSize, "DDS read failed");
        Log("[moon-main] DDS read failed size=%ld path=%s\n", size, path.c_str());
        return false;
    }
    fclose(f);

    if (memcmp(file.data(), "DDS ", 4) != 0 || ReadU32(file.data() + 4) != 124 || ReadU32(file.data() + 76) != 32) {
        SetLoadStatus(status, statusSize, "invalid DDS header");
        Log("[moon-main] DDS header invalid path=%s\n", path.c_str());
        return false;
    }

    outImage.width = ReadU32(file.data() + 16);
    outImage.height = ReadU32(file.data() + 12);
    outImage.mips = std::max(1u, ReadU32(file.data() + 28));
    const uint32_t pfFlags = ReadU32(file.data() + 80);
    const uint32_t fourcc = ReadU32(file.data() + 84);
    if ((pfFlags & 4) == 0) {
        SetLoadStatus(status, statusSize, "unsupported DDS format");
        Log("[moon-main] DDS is not FourCC compressed pfFlags=0x%X path=%s\n", pfFlags, path.c_str());
        return false;
    }

    if (fourcc == ReadU32(reinterpret_cast<const uint8_t*>("DXT1"))) {
        outImage.format = DXGI_FORMAT_BC1_UNORM;
    } else if (fourcc == ReadU32(reinterpret_cast<const uint8_t*>("DXT5"))) {
        outImage.format = DXGI_FORMAT_BC3_UNORM;
    } else {
        char cc[5]{ static_cast<char>(fourcc & 0xFF), static_cast<char>((fourcc >> 8) & 0xFF), static_cast<char>((fourcc >> 16) & 0xFF), static_cast<char>((fourcc >> 24) & 0xFF), 0 };
        snprintf(status, statusSize, "unsupported DDS %s", cc);
        Log("[moon-main] DDS unsupported fourcc=%s path=%s\n", cc, path.c_str());
        return false;
    }

    const UINT blockBytes = outImage.format == DXGI_FORMAT_BC1_UNORM ? 8u : 16u;
    outImage.sourceRows = std::max(1u, (outImage.height + 3u) / 4u);
    outImage.sourceRowPitch = static_cast<UINT64>((outImage.width + 3u) / 4u) * blockBytes;
    const UINT64 requiredBytes = outImage.sourceRowPitch * outImage.sourceRows;
    outImage.pixels.assign(file.begin() + 128, file.end());
    if (outImage.pixels.size() < requiredBytes) {
        SetLoadStatus(status, statusSize, "DDS pixel data too small");
        Log("[moon-main] DDS pixel data too small have=%zu need=%llu\n", outImage.pixels.size(), static_cast<unsigned long long>(requiredBytes));
        return false;
    }

    Log("[moon-main] loaded DDS %s %ux%u mips=%u fmt=%s(%u) bytes=%zu\n",
        path.c_str(),
        outImage.width,
        outImage.height,
        outImage.mips,
        FormatName(outImage.format),
        static_cast<unsigned>(outImage.format),
        outImage.pixels.size());
    return true;
}

bool LoadPngFile(const std::string& path, MoonImageData& outImage, char* status, size_t statusSize) {
    const std::wstring widePath = Utf8ToWide(path);
    if (widePath.empty()) {
        SetLoadStatus(status, statusSize, "PNG path conversion failed");
        Log("[moon-main] PNG path conversion failed path=%s\n", path.c_str());
        return false;
    }

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
        SetLoadStatus(status, statusSize, "PNG COM init failed");
        Log("[moon-main] PNG COM init failed hr=0x%08X path=%s\n", static_cast<unsigned>(coHr), path.c_str());
        return false;
    }

    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) {
        if (coInitialized) CoUninitialize();
        SetLoadStatus(status, statusSize, "PNG decoder unavailable");
        Log("[moon-main] WIC factory failed hr=0x%08X path=%s\n", static_cast<unsigned>(hr), path.c_str());
        return false;
    }

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr) || !decoder) {
        factory->Release();
        if (coInitialized) CoUninitialize();
        SetLoadStatus(status, statusSize, "PNG read failed");
        Log("[moon-main] PNG decoder open failed hr=0x%08X path=%s\n", static_cast<unsigned>(hr), path.c_str());
        return false;
    }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
        decoder->Release();
        factory->Release();
        if (coInitialized) CoUninitialize();
        SetLoadStatus(status, statusSize, "PNG frame failed");
        Log("[moon-main] PNG frame failed hr=0x%08X path=%s\n", static_cast<unsigned>(hr), path.c_str());
        return false;
    }

    UINT width = 0;
    UINT height = 0;
    frame->GetSize(&width, &height);
    if (width == 0 || height == 0 || width > 4096 || height > 4096) {
        frame->Release();
        decoder->Release();
        factory->Release();
        if (coInitialized) CoUninitialize();
        SetLoadStatus(status, statusSize, "PNG size unsupported");
        Log("[moon-main] PNG size unsupported %ux%u path=%s\n", width, height, path.c_str());
        return false;
    }

    IWICFormatConverter* converter = nullptr;
    hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr) && converter) {
        hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    if (FAILED(hr) || !converter) {
        if (converter) converter->Release();
        frame->Release();
        decoder->Release();
        factory->Release();
        if (coInitialized) CoUninitialize();
        SetLoadStatus(status, statusSize, "PNG convert failed");
        Log("[moon-main] PNG convert failed hr=0x%08X path=%s\n", static_cast<unsigned>(hr), path.c_str());
        return false;
    }

    outImage.width = width;
    outImage.height = height;
    outImage.mips = 1;
    outImage.sourceRows = height;
    outImage.sourceRowPitch = static_cast<UINT64>(width) * 4u;
    outImage.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    outImage.pixels.resize(static_cast<size_t>(outImage.sourceRowPitch) * outImage.sourceRows);
    hr = converter->CopyPixels(nullptr, static_cast<UINT>(outImage.sourceRowPitch), static_cast<UINT>(outImage.pixels.size()), outImage.pixels.data());

    converter->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
    if (coInitialized) CoUninitialize();

    if (FAILED(hr)) {
        outImage = MoonImageData{};
        SetLoadStatus(status, statusSize, "PNG copy failed");
        Log("[moon-main] PNG copy failed hr=0x%08X path=%s\n", static_cast<unsigned>(hr), path.c_str());
        return false;
    }

    Log("[moon-main] loaded PNG %s %ux%u fmt=%s(%u) bytes=%zu\n",
        path.c_str(),
        outImage.width,
        outImage.height,
        FormatName(outImage.format),
        static_cast<unsigned>(outImage.format),
        outImage.pixels.size());
    return true;
}

} // namespace

bool LoadMoonImageFile(const std::string& path, MoonImageData& outImage, char* status, size_t statusSize) {
    outImage = MoonImageData{};
    if (path.empty()) {
        SetLoadStatus(status, statusSize, "Native");
        return false;
    }

    if (EndsWithNoCase(path, ".dds")) {
        return LoadDdsFile(path, outImage, status, statusSize);
    }
    if (EndsWithNoCase(path, ".png")) {
        return LoadPngFile(path, outImage, status, statusSize);
    }

    SetLoadStatus(status, statusSize, "unsupported moon texture file");
    Log("[moon-main] unsupported moon texture path=%s\n", path.c_str());
    return false;
}
