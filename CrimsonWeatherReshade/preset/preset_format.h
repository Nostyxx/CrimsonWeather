#pragma once

#include "preset_model.h"

namespace preset_internal {

struct PresetFormatOptions {
    bool extendedSliderRange = false;
};

bool IsValidPresetFile(const char* path);
bool LoadPresetPackageInternal(const char* path, const PresetFormatOptions& options, WeatherPresetPackage& outPackage);
bool WritePresetPackageInternal(const char* path, const PresetFormatOptions& options, const WeatherPresetPackage& package);
bool WritePresetPackageWithCommunityMetadata(
    const char* path,
    const PresetFormatOptions& options,
    const WeatherPresetPackage& package,
    const char* catalogId,
    const char* sha256,
    const char* updatedAt);
bool ReadCommunityMetadataFromPresetFile(const char* path, CommunityPresetInstallInfo& outInfo);
std::string SerializePresetPackage(const WeatherPresetPackage& package, const PresetFormatOptions& options);

} // namespace preset_internal
