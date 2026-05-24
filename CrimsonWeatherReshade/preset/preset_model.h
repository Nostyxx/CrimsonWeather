#pragma once

#include "preset_service.h"

#include <array>
#include <string>

namespace preset_internal {

inline constexpr const char* kPresetHeader = "[CrimsonWeatherPreset]";
inline constexpr float kCloudScatteringCoefficientMin = 0.00001f;

struct PresetListItem {
    std::string fileName;
    std::string displayName;
    std::string fullPath;
};

struct WeatherPresetMask {
    bool forceClearSky = false;
    bool noRain = false;
    bool rain = false;
    bool thunder = false;
    bool noDust = false;
    bool dust = false;
    bool noSnow = false;
    bool snow = false;
    bool snowAccumBoundaryA = false;
    bool snowAccumBoundaryB = false;
    bool snowCoverageThreshold = false;
    bool time = false;
    bool cloudAmount = false;
    bool cloudHeight = false;
    bool cloudDensity = false;
    bool midClouds = false;
    bool highClouds = false;
    bool cloudAlpha = false;
    bool cloudFadeRange = false;
    bool cloudDetailRatio = false;
    bool cloudPhaseFront = false;
    bool cloudScatteringCoefficient = false;
    bool cloudFlow = false;
    bool cloudVisibleRange = false;
    bool rayleighHeight = false;
    bool ozoneRatio = false;
    bool rayleighScatteringColor = false;
    bool exp2C = false;
    bool exp2D = false;
    bool cloudVariation = false;
    bool nightSkyRotation = false;
    bool nightSkyYaw = false;
    bool sunSize = false;
    bool sunLightIntensity = false;
    bool sunYaw = false;
    bool sunPitch = false;
    bool moonSize = false;
    bool moonLightIntensity = false;
    bool moonYaw = false;
    bool moonPitch = false;
    bool moonRoll = false;
    bool moonTexture = false;
    bool milkywayTexture = false;
    bool fog = false;
    bool nativeFog = false;
    bool volumeFogScatterColor = false;
    bool mieScatterColor = false;
    bool mieScaleHeight = false;
    bool mieAerosolDensity = false;
    bool mieAerosolAbsorption = false;
    bool heightFogBaseline = false;
    bool heightFogFalloff = false;
    bool noFog = false;
    bool wind = false;
    bool noWind = false;
    bool puddleScale = false;
};

struct WeatherPresetPackage {
    WeatherPresetData global{};
    std::array<bool, kPresetRegionCount> regionEnabled{};
    std::array<WeatherPresetData, kPresetRegionCount> region{};
    std::array<WeatherPresetMask, kPresetRegionCount> regionMask{};
};

float ClampPresetFloat(float value, float lo, float hi);
float ClampPresetRain(bool extendedSliderRange, float value);
float ClampPresetThunder(bool extendedSliderRange, float value);
float ClampPresetDust(bool extendedSliderRange, float value);
float ClampPresetSnow(bool extendedSliderRange, float value);
float ClampPresetSnowBoundary(float value);
float ClampPresetCloudAmount(bool extendedSliderRange, float value);
float ClampPresetCloudHeight(bool extendedSliderRange, float value);
float ClampPresetCloudDensity(bool extendedSliderRange, float value);
float ClampPresetCloudWide(bool extendedSliderRange, float value);
float ClampPresetFogPercent(bool extendedSliderRange, float value);
float ClampPresetNativeFog(bool extendedSliderRange, float value);
float ClampPresetWind(bool extendedSliderRange, float value);
float ClampPresetPuddleScale(bool extendedSliderRange, float value);
float ClampPresetPitch(bool extendedSliderRange, float value);
float ClampPresetYaw(bool extendedSliderRange, float value);
float ClampPresetSunSize(bool extendedSliderRange, float value);
float ClampPresetMoonSize(bool extendedSliderRange, float value);
float ClampPresetLightIntensity(bool extendedSliderRange, float value);
float ClampPresetMieScaleHeight(bool extendedSliderRange, float value);
float ClampPresetMieDensity(bool extendedSliderRange, float value);
float ClampPresetMieAbsorption(bool extendedSliderRange, float value);
float ClampPresetHeightFogBaseline(bool extendedSliderRange, float value);
float ClampPresetHeightFogFalloff(bool extendedSliderRange, float value);
float ClampPresetCloudAlpha(bool extendedSliderRange, float value);
float ClampPresetCloudFadeRange(bool extendedSliderRange, float value);
float ClampPresetCloudDetailRatio(float value);
float ClampPresetCloudPhaseFront(bool extendedSliderRange, float value);
float ClampPresetCloudScatteringCoefficient(bool extendedSliderRange, float value);
float ClampPresetCloudFlow(bool extendedSliderRange, float value);
float ClampPresetCloudVisibleRange(float value);
float ClampPresetRayleighHeight(bool extendedSliderRange, float value);
float ClampPresetOzoneRatio(bool extendedSliderRange, float value);
WeatherPresetColor ClampPresetColor(WeatherPresetColor color, bool includeAlpha);

bool FloatNearlyEqual(float a, float b, float epsilon = 0.0005f);
bool HourNearlyEqual(float a, float b, float epsilon = 0.0005f);
bool PresetDataEquals(const WeatherPresetData& a, const WeatherPresetData& b);
bool PresetMaskAny(const WeatherPresetMask& mask);
WeatherPresetSourceMask ToSourceMask(const WeatherPresetMask& mask);
WeatherPresetMask FromSourceMask(const WeatherPresetSourceMask& source);
bool PresetMaskEquals(const WeatherPresetMask& a, const WeatherPresetMask& b);
WeatherPresetMask BuildOverrideMask(const WeatherPresetData& base, const WeatherPresetData& value);
void ApplyPresetMask(WeatherPresetData& target, const WeatherPresetData& source, const WeatherPresetMask& mask);
WeatherPresetData BlendPresetData(const WeatherPresetData& a, const WeatherPresetData& b, float t);
bool IsPresetRegionId(int regionId);
const char* RegionToken(int regionId);
const char* RegionDisplayName(int regionId);
int RegionIdFromToken(const std::string& token);
int CurrentMajorRegionForPreset();
WeatherPresetData EffectivePresetDataForRegion(const WeatherPresetPackage& package, int regionId);
bool PresetPackageEquals(const WeatherPresetPackage& a, const WeatherPresetPackage& b);

std::string TrimCopy(const std::string& value);
void StripUtf8Bom(std::string& value);
bool EqualsNoCase(const std::string& a, const std::string& b);
bool EndsWithIni(const std::string& value);
std::string EnsureIniExtension(const std::string& value);
std::string GetPresetDisplayNameFromFileName(const std::string& fileName);
std::string GetPresetDirectory();
std::string JoinPath(const std::string& dir, const std::string& fileName);
std::string GetCommunityPresetDirectory();

} // namespace preset_internal
