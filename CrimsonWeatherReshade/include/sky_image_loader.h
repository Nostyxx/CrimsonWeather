#pragma once

#include <Windows.h>
#include <dxgi.h>

#include <cstdint>
#include <string>
#include <vector>

struct SkyImageData {
    struct MipLevel {
        UINT width = 0;
        UINT height = 0;
        UINT sourceRows = 0;
        UINT64 sourceRowPitch = 0;
        size_t byteOffset = 0;
        size_t byteSize = 0;
    };

    std::vector<uint8_t> pixels;
    std::vector<MipLevel> mipLevels;
    UINT width = 0;
    UINT height = 0;
    UINT mips = 1;
    UINT sourceRows = 0;
    UINT64 sourceRowPitch = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

bool LoadSkyImageFile(const std::string& path, SkyImageData& outImage, char* status, size_t statusSize);
