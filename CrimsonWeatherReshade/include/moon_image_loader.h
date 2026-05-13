#pragma once

#include <Windows.h>
#include <dxgi.h>

#include <cstdint>
#include <string>
#include <vector>

struct MoonImageData {
    std::vector<uint8_t> pixels;
    UINT width = 0;
    UINT height = 0;
    UINT mips = 1;
    UINT sourceRows = 0;
    UINT64 sourceRowPitch = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

bool LoadMoonImageFile(const std::string& path, MoonImageData& outImage, char* status, size_t statusSize);
