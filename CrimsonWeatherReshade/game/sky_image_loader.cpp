#include "pch.h"

#include "sky_image_loader.h"
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
        case DXGI_FORMAT_BC1_TYPELESS: return "BC1_TYPELESS";
        case DXGI_FORMAT_BC1_UNORM: return "BC1_UNORM";
        case DXGI_FORMAT_BC1_UNORM_SRGB: return "BC1_UNORM_SRGB";
        case DXGI_FORMAT_BC2_TYPELESS: return "BC2_TYPELESS";
        case DXGI_FORMAT_BC2_UNORM: return "BC2_UNORM";
        case DXGI_FORMAT_BC2_UNORM_SRGB: return "BC2_UNORM_SRGB";
        case DXGI_FORMAT_BC3_TYPELESS: return "BC3_TYPELESS";
        case DXGI_FORMAT_BC3_UNORM: return "BC3_UNORM";
        case DXGI_FORMAT_BC3_UNORM_SRGB: return "BC3_UNORM_SRGB";
        case DXGI_FORMAT_BC4_TYPELESS: return "BC4_TYPELESS";
        case DXGI_FORMAT_BC4_UNORM: return "BC4_UNORM";
        case DXGI_FORMAT_BC4_SNORM: return "BC4_SNORM";
        case DXGI_FORMAT_BC5_TYPELESS: return "BC5_TYPELESS";
        case DXGI_FORMAT_BC5_UNORM: return "BC5_UNORM";
        case DXGI_FORMAT_BC5_SNORM: return "BC5_SNORM";
        case DXGI_FORMAT_BC6H_TYPELESS: return "BC6H_TYPELESS";
        case DXGI_FORMAT_BC6H_UF16: return "BC6H_UF16";
        case DXGI_FORMAT_BC6H_SF16: return "BC6H_SF16";
        case DXGI_FORMAT_BC7_TYPELESS: return "BC7_TYPELESS";
        case DXGI_FORMAT_BC7_UNORM: return "BC7_UNORM";
        case DXGI_FORMAT_BC7_UNORM_SRGB: return "BC7_UNORM_SRGB";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        default: return "other";
        }
    }

    bool IsBlockCompressedFormat(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return true;
        default:
            return false;
        }
    }

    UINT BlockBytesForFormat(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_BC1_TYPELESS:
        case DXGI_FORMAT_BC1_UNORM:
        case DXGI_FORMAT_BC1_UNORM_SRGB:
        case DXGI_FORMAT_BC4_TYPELESS:
        case DXGI_FORMAT_BC4_UNORM:
        case DXGI_FORMAT_BC4_SNORM:
            return 8u;
        case DXGI_FORMAT_BC2_TYPELESS:
        case DXGI_FORMAT_BC2_UNORM:
        case DXGI_FORMAT_BC2_UNORM_SRGB:
        case DXGI_FORMAT_BC3_TYPELESS:
        case DXGI_FORMAT_BC3_UNORM:
        case DXGI_FORMAT_BC3_UNORM_SRGB:
        case DXGI_FORMAT_BC5_TYPELESS:
        case DXGI_FORMAT_BC5_UNORM:
        case DXGI_FORMAT_BC5_SNORM:
        case DXGI_FORMAT_BC6H_TYPELESS:
        case DXGI_FORMAT_BC6H_UF16:
        case DXGI_FORMAT_BC6H_SF16:
        case DXGI_FORMAT_BC7_TYPELESS:
        case DXGI_FORMAT_BC7_UNORM:
        case DXGI_FORMAT_BC7_UNORM_SRGB:
            return 16u;
        default:
            return 0u;
        }
    }

    UINT BytesPerPixelForFormat(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return 4u;
        default:
            return 0u;
        }
    }

    bool AddMipLevel(SkyImageData& outImage,
                     UINT width,
                     UINT height,
                     UINT sourceRows,
                     UINT64 sourceRowPitch,
                     size_t byteOffset,
                     size_t byteSize,
                     char* status,
                     size_t statusSize) {
        if (width == 0 || height == 0 || sourceRows == 0 || sourceRowPitch == 0) {
            SetLoadStatus(status, statusSize, "invalid mip dimensions");
            return false;
        }

        SkyImageData::MipLevel mip{};
        mip.width = width;
        mip.height = height;
        mip.sourceRows = sourceRows;
        mip.sourceRowPitch = sourceRowPitch;
        mip.byteOffset = byteOffset;
        mip.byteSize = byteSize;
        outImage.mipLevels.push_back(mip);
        return true;
    }

    UINT MaxMipLevelsForSize(UINT width, UINT height) {
        UINT levels = 1;
        width = std::max(1u, width);
        height = std::max(1u, height);
        while (width > 1 || height > 1) {
            width = std::max(1u, width / 2u);
            height = std::max(1u, height / 2u);
            ++levels;
        }
        return levels;
    }

    bool GenerateRgba8MipChain(SkyImageData& image, char* status, size_t statusSize) {
        if (image.format != DXGI_FORMAT_R8G8B8A8_UNORM || image.mipLevels.empty()) {
            return true;
        }

        UINT prevWidth = image.width;
        UINT prevHeight = image.height;
        size_t prevOffset = 0;
        UINT64 prevRowPitch = static_cast<UINT64>(prevWidth) * 4u;
        while (prevWidth > 1 || prevHeight > 1) {
            const UINT nextWidth = std::max(1u, prevWidth / 2u);
            const UINT nextHeight = std::max(1u, prevHeight / 2u);
            const UINT64 nextRowPitch = static_cast<UINT64>(nextWidth) * 4u;
            const size_t nextOffset = image.pixels.size();
            const UINT64 nextBytes64 = nextRowPitch * nextHeight;
            if (nextBytes64 > static_cast<UINT64>(SIZE_MAX)) {
                SetLoadStatus(status, statusSize, "PNG mip chain too large");
                return false;
            }
            image.pixels.resize(image.pixels.size() + static_cast<size_t>(nextBytes64));

            const uint8_t* prev = image.pixels.data() + prevOffset;
            uint8_t* next = image.pixels.data() + nextOffset;
            for (UINT y = 0; y < nextHeight; ++y) {
                for (UINT x = 0; x < nextWidth; ++x) {
                    uint32_t sum[4] = {};
                    uint32_t count = 0;
                    for (UINT oy = 0; oy < 2; ++oy) {
                        const UINT sy = y * 2u + oy;
                        if (sy >= prevHeight) {
                            continue;
                        }
                        for (UINT ox = 0; ox < 2; ++ox) {
                            const UINT sx = x * 2u + ox;
                            if (sx >= prevWidth) {
                                continue;
                            }
                            const uint8_t* src = prev + static_cast<size_t>(sy) * prevRowPitch + static_cast<size_t>(sx) * 4u;
                            sum[0] += src[0];
                            sum[1] += src[1];
                            sum[2] += src[2];
                            sum[3] += src[3];
                            ++count;
                        }
                    }
                    count = std::max(1u, count);
                    uint8_t* dst = next + static_cast<size_t>(y) * nextRowPitch + static_cast<size_t>(x) * 4u;
                    dst[0] = static_cast<uint8_t>((sum[0] + count / 2u) / count);
                    dst[1] = static_cast<uint8_t>((sum[1] + count / 2u) / count);
                    dst[2] = static_cast<uint8_t>((sum[2] + count / 2u) / count);
                    dst[3] = static_cast<uint8_t>((sum[3] + count / 2u) / count);
                }
            }

            if (!AddMipLevel(image, nextWidth, nextHeight, nextHeight, nextRowPitch, nextOffset, static_cast<size_t>(nextBytes64), status, statusSize)) {
                return false;
            }
            prevWidth = nextWidth;
            prevHeight = nextHeight;
            prevOffset = nextOffset;
            prevRowPitch = nextRowPitch;
        }

        image.mips = static_cast<UINT>(image.mipLevels.size());
        image.sourceRows = image.mipLevels[0].sourceRows;
        image.sourceRowPitch = image.mipLevels[0].sourceRowPitch;
        return true;
    }

    bool LoadDdsFile(const std::string& path, SkyImageData& outImage, char* status, size_t statusSize) {
        FILE* f = nullptr;
        fopen_s(&f, path.c_str(), "rb");
        if (!f) {
            SetLoadStatus(status, statusSize, "selected DDS missing");
            Log("[sky-image] selected DDS missing path=%s\n", path.c_str());
            return false;
        }

        fseek(f, 0, SEEK_END);
        const long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (size <= 128) {
            fclose(f);
            SetLoadStatus(status, statusSize, "DDS too small");
            Log("[sky-image] DDS too small size=%ld path=%s\n", size, path.c_str());
            return false;
        }

        std::vector<uint8_t> file(static_cast<size_t>(size));
        if (fread(file.data(), 1, file.size(), f) != file.size()) {
            fclose(f);
            SetLoadStatus(status, statusSize, "DDS read failed");
            Log("[sky-image] DDS read failed size=%ld path=%s\n", size, path.c_str());
            return false;
        }
        fclose(f);

        if (memcmp(file.data(), "DDS ", 4) != 0 || ReadU32(file.data() + 4) != 124 || ReadU32(file.data() + 76) != 32) {
            SetLoadStatus(status, statusSize, "invalid DDS header");
            Log("[sky-image] DDS header invalid path=%s\n", path.c_str());
            return false;
        }

        outImage.width = ReadU32(file.data() + 16);
        outImage.height = ReadU32(file.data() + 12);
        outImage.mips = std::max(1u, ReadU32(file.data() + 28));
        if (outImage.width == 0 || outImage.height == 0 || outImage.mips > MaxMipLevelsForSize(outImage.width, outImage.height)) {
            SetLoadStatus(status, statusSize, "DDS size unsupported");
            Log("[sky-image] DDS size unsupported %ux%u mips=%u path=%s\n", outImage.width, outImage.height, outImage.mips, path.c_str());
            return false;
        }
        const uint32_t pfFlags = ReadU32(file.data() + 80);
        const uint32_t fourcc = ReadU32(file.data() + 84);
        const uint32_t rgbBitCount = ReadU32(file.data() + 88);
        const uint32_t rMask = ReadU32(file.data() + 92);
        const uint32_t gMask = ReadU32(file.data() + 96);
        const uint32_t bMask = ReadU32(file.data() + 100);
        const uint32_t aMask = ReadU32(file.data() + 104);

        size_t dataOffset = 128;
        if ((pfFlags & 4) != 0 && fourcc == ReadU32(reinterpret_cast<const uint8_t*>("DXT1"))) {
            outImage.format = DXGI_FORMAT_BC1_UNORM;
        }
        else if ((pfFlags & 4) != 0 && fourcc == ReadU32(reinterpret_cast<const uint8_t*>("DXT3"))) {
            outImage.format = DXGI_FORMAT_BC2_UNORM;
        }
        else if ((pfFlags & 4) != 0 && fourcc == ReadU32(reinterpret_cast<const uint8_t*>("DXT5"))) {
            outImage.format = DXGI_FORMAT_BC3_UNORM;
        }
        else if ((pfFlags & 4) != 0 && fourcc == ReadU32(reinterpret_cast<const uint8_t*>("DX10"))) {
            if (file.size() < 148) {
                SetLoadStatus(status, statusSize, "DDS DX10 header too small");
                Log("[sky-image] DDS DX10 header too small path=%s\n", path.c_str());
                return false;
            }
            const DXGI_FORMAT dx10Format = static_cast<DXGI_FORMAT>(ReadU32(file.data() + 128));
            const uint32_t resourceDimension = ReadU32(file.data() + 132);
            const uint32_t arraySize = ReadU32(file.data() + 140);
            if (resourceDimension != 3 || arraySize == 0 || arraySize > 1) {
                SetLoadStatus(status, statusSize, "DDS DX10 dimension unsupported");
                Log("[sky-image] DDS DX10 unsupported dimension=%u array=%u path=%s\n", resourceDimension, arraySize, path.c_str());
                return false;
            }
            if (!IsBlockCompressedFormat(dx10Format) && BytesPerPixelForFormat(dx10Format) == 0) {
                SetLoadStatus(status, statusSize, "DDS DX10 format unsupported");
                Log("[sky-image] DDS DX10 unsupported format=%s(%u) path=%s\n",
                    FormatName(dx10Format),
                    static_cast<unsigned>(dx10Format),
                    path.c_str());
                return false;
            }
            outImage.format = dx10Format;
            dataOffset = 148;
        }
        else if ((pfFlags & 0x40) != 0 && rgbBitCount == 32 &&
                 rMask == 0x00FF0000u && gMask == 0x0000FF00u &&
                 bMask == 0x000000FFu && aMask == 0xFF000000u) {
            outImage.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        else if ((pfFlags & 0x40) != 0 && rgbBitCount == 32 &&
                 rMask == 0x000000FFu && gMask == 0x0000FF00u &&
                 bMask == 0x00FF0000u && aMask == 0xFF000000u) {
            outImage.format = DXGI_FORMAT_R8G8B8A8_UNORM;
        }
        else {
            char cc[5]{ static_cast<char>(fourcc & 0xFF), static_cast<char>((fourcc >> 8) & 0xFF), static_cast<char>((fourcc >> 16) & 0xFF), static_cast<char>((fourcc >> 24) & 0xFF), 0 };
            snprintf(status, statusSize, "unsupported DDS %s", (pfFlags & 4) != 0 ? cc : "format");
            Log("[sky-image] DDS unsupported pfFlags=0x%X fourcc=%s rgbBits=%u masks=%08X/%08X/%08X/%08X path=%s\n",
                pfFlags,
                cc,
                rgbBitCount,
                rMask,
                gMask,
                bMask,
                aMask,
                path.c_str());
            return false;
        }

        outImage.pixels.assign(file.begin() + dataOffset, file.end());

        const UINT blockBytes = BlockBytesForFormat(outImage.format);
        const UINT bytesPerPixel = BytesPerPixelForFormat(outImage.format);
        UINT mipWidth = std::max(1u, outImage.width);
        UINT mipHeight = std::max(1u, outImage.height);
        size_t byteOffset = 0;
        for (UINT mip = 0; mip < outImage.mips; ++mip) {
            const bool compressed = blockBytes != 0;
            const UINT sourceRows = compressed ? std::max(1u, (mipHeight + 3u) / 4u) : mipHeight;
            const UINT64 sourceRowPitch = compressed
                ? static_cast<UINT64>(std::max(1u, (mipWidth + 3u) / 4u)) * blockBytes
                : static_cast<UINT64>(mipWidth) * bytesPerPixel;
            const UINT64 mipBytes64 = sourceRowPitch * sourceRows;
            if (mipBytes64 > static_cast<UINT64>(SIZE_MAX) || byteOffset > outImage.pixels.size() ||
                static_cast<UINT64>(outImage.pixels.size() - byteOffset) < mipBytes64) {
                SetLoadStatus(status, statusSize, "DDS pixel data too small");
                Log("[sky-image] DDS mip data too small mip=%u have=%zu need=%llu total=%zu path=%s\n",
                    mip,
                    byteOffset <= outImage.pixels.size() ? outImage.pixels.size() - byteOffset : 0,
                    static_cast<unsigned long long>(mipBytes64),
                    outImage.pixels.size(),
                    path.c_str());
                return false;
            }
            if (!AddMipLevel(outImage, mipWidth, mipHeight, sourceRows, sourceRowPitch, byteOffset, static_cast<size_t>(mipBytes64), status, statusSize)) {
                return false;
            }
            byteOffset += static_cast<size_t>(mipBytes64);
            mipWidth = std::max(1u, mipWidth / 2u);
            mipHeight = std::max(1u, mipHeight / 2u);
        }
        outImage.sourceRows = outImage.mipLevels[0].sourceRows;
        outImage.sourceRowPitch = outImage.mipLevels[0].sourceRowPitch;

        Log("[sky-image] loaded DDS %s %ux%u mips=%u fmt=%s(%u) bytes=%zu\n",
            path.c_str(),
            outImage.width,
            outImage.height,
            outImage.mips,
            FormatName(outImage.format),
            static_cast<unsigned>(outImage.format),
            outImage.pixels.size());
        return true;
    }

    bool LoadPngFile(const std::string& path, SkyImageData& outImage, char* status, size_t statusSize) {
        const std::wstring widePath = Utf8ToWide(path);
        if (widePath.empty()) {
            SetLoadStatus(status, statusSize, "PNG path conversion failed");
            Log("[sky-image] PNG path conversion failed path=%s\n", path.c_str());
            return false;
        }

        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool coInitialized = SUCCEEDED(coHr);
        if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
            SetLoadStatus(status, statusSize, "PNG COM init failed");
            Log("[sky-image] PNG COM init failed hr=0x%08X path=%s\n", static_cast<unsigned>(coHr), path.c_str());
            return false;
        }

        IWICImagingFactory* factory = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory) {
            if (coInitialized) CoUninitialize();
            SetLoadStatus(status, statusSize, "PNG decoder unavailable");
            Log("[sky-image] WIC factory failed hr=0x%08X path=%s\n", static_cast<unsigned>(hr), path.c_str());
            return false;
        }

        IWICBitmapDecoder* decoder = nullptr;
        hr = factory->CreateDecoderFromFilename(widePath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
        if (FAILED(hr) || !decoder) {
            factory->Release();
            if (coInitialized) CoUninitialize();
            SetLoadStatus(status, statusSize, "PNG read failed");
            Log("[sky-image] PNG decoder open failed hr=0x%08X path=%s\n", static_cast<unsigned>(hr), path.c_str());
            return false;
        }

        IWICBitmapFrameDecode* frame = nullptr;
        hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr) || !frame) {
            decoder->Release();
            factory->Release();
            if (coInitialized) CoUninitialize();
            SetLoadStatus(status, statusSize, "PNG frame failed");
            Log("[sky-image] PNG frame failed hr=0x%08X path=%s\n", static_cast<unsigned>(hr), path.c_str());
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
            Log("[sky-image] PNG size unsupported %ux%u path=%s\n", width, height, path.c_str());
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
            Log("[sky-image] PNG convert failed hr=0x%08X path=%s\n", static_cast<unsigned>(hr), path.c_str());
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
            outImage = SkyImageData{};
            SetLoadStatus(status, statusSize, "PNG copy failed");
            Log("[sky-image] PNG copy failed hr=0x%08X path=%s\n", static_cast<unsigned>(hr), path.c_str());
            return false;
        }
        if (!AddMipLevel(outImage, width, height, outImage.sourceRows, outImage.sourceRowPitch, 0, outImage.pixels.size(), status, statusSize) ||
            !GenerateRgba8MipChain(outImage, status, statusSize)) {
            outImage = SkyImageData{};
            Log("[sky-image] PNG mip generation failed path=%s status=%s\n", path.c_str(), status ? status : "");
            return false;
        }

        Log("[sky-image] loaded PNG %s %ux%u mips=%u fmt=%s(%u) bytes=%zu\n",
            path.c_str(),
            outImage.width,
            outImage.height,
            outImage.mips,
            FormatName(outImage.format),
            static_cast<unsigned>(outImage.format),
            outImage.pixels.size());
        return true;
    }

} // namespace

bool LoadSkyImageFile(const std::string& path, SkyImageData& outImage, char* status, size_t statusSize) {
    outImage = SkyImageData{};
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

    SetLoadStatus(status, statusSize, "unsupported sky texture file");
    Log("[sky-image] unsupported texture path=%s\n", path.c_str());
    return false;
}
