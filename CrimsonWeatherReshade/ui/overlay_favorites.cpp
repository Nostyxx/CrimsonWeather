#include "pch.h"

#include "overlay_internal.h"
#include "preset_service.h"
#include "runtime_shared.h"
#include "sky_texture_override.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace overlay_internal {
namespace {

constexpr float kNativeEpsilon = 0.001f;
constexpr float kCloudScatteringCoefficientMin = 0.00001f;
constexpr int kMaxFavoriteSections = 32;
constexpr int kMaxFavoriteControlsPerSection = 64;

enum class FavoriteNativeSource {
    Zero,
    One,
    SnowBoundaryA,
    SnowBoundaryB,
    SnowCoverageThreshold,
    RayleighHeight,
    OzoneRatio,
    CloudAlpha,
    CloudFadeRange,
    CloudDetailRatio,
    CloudPhaseFront,
    CloudScattering,
    CloudFlow,
    AerosolHeight,
    AerosolDensity,
    AerosolAbsorption,
    FogBaseline,
    FogFalloff,
    NightSkyTilt,
    NightSkyPhase,
    SunLightIntensity,
    SunSize,
    SunYaw,
    SunPitch,
    MoonLightIntensity,
    MoonSize,
    MoonYaw,
    MoonPitch,
    MoonRotation
};

enum class FavoritePresetMode {
    ValueOnly,
    EnabledAlways,
    EnabledAwayFromNative
};

enum class FavoriteRuntimeMode {
    OverrideClearAtNative,
    OverrideAlways,
    WindMultiplier,
    LegacyFog
};

enum class FavoriteGate {
    Rain,
    Dust,
    Snow,
    SnowAdvanced,
    Thunder,
    Wind,
    WindPack,
    Cloud,
    FogFromWind,
    CelestialWind,
    CelestialScene,
    Experiment,
    LegacyFog,
    Detail
};

enum class FavoriteExtraKind {
    ForceClear,
    NoRain,
    NoDust,
    NoSnow,
    NoWind,
    NoFog,
    RayleighColor,
    VolumeFogColor,
    MieColor,
    MilkywayTexture,
    MoonTexture,
    TimeControls
};

struct FavoriteSliderSpec {
    const char* id;
    const char* category;
    const char* label;
    float normalLo;
    float normalHi;
    float extendedLo;
    float extendedHi;
    const char* format;
    float WeatherPresetData::* valueField;
    bool WeatherPresetData::* enabledField;
    bool WeatherPresetSourceMask::* maskField;
    SliderOverride* runtimeValue;
    FavoriteNativeSource nativeSource;
    FavoritePresetMode presetMode;
    FavoriteRuntimeMode runtimeMode;
    FavoriteGate gate;
};

struct FavoriteExtraSpec {
    const char* id;
    const char* category;
    const char* label;
    FavoriteExtraKind kind;
};

struct FavoriteSection {
    char name[64] = "New Section";
    std::vector<std::string> controlIds;
};

std::vector<FavoriteSection> g_sections;
std::vector<FavoriteSection> g_editorSections;
bool g_layoutLoaded = false;
int g_selectedEditorSection = -1;
int g_selectedEditorControl = -1;
bool g_focusEditorSectionName = false;
bool g_openFavoritesEditorRequest = false;
char g_controlFilter[96] = "";
char g_favoriteMoonTextureFilter[96] = "";
char g_favoriteMilkywayTextureFilter[96] = "";
DWORD64 g_lastFavoritesTickMs = 0;

const FavoriteSliderSpec kSliderSpecs[] = {
    { "rain", "Weather", "Rain", 0.0f, 1.0f, 0.0f, 5.0f, "%.3f", &WeatherPresetData::rain, nullptr, &WeatherPresetSourceMask::rain, &g_oRain, FavoriteNativeSource::Zero, FavoritePresetMode::ValueOnly, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::Rain },
    { "dust", "Weather", "Dust", 0.0f, 2.0f, 0.0f, 10.0f, "%.3f", &WeatherPresetData::dust, nullptr, &WeatherPresetSourceMask::dust, &g_oDust, FavoriteNativeSource::Zero, FavoritePresetMode::ValueOnly, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::Dust },
    { "snow", "Weather", "Snow", 0.0f, 1.0f, 0.0f, 5.0f, "%.3f", &WeatherPresetData::snow, nullptr, &WeatherPresetSourceMask::snow, &g_oSnow, FavoriteNativeSource::Zero, FavoritePresetMode::ValueOnly, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::Snow },
    { "snow_accum_boundary_a", "Weather", "Accumulation Boundary A", -1000.0f, 1500.0f, -1000.0f, 1500.0f, "%.1f", &WeatherPresetData::snowAccumBoundaryA, &WeatherPresetData::snowAccumBoundaryAEnabled, &WeatherPresetSourceMask::snowAccumBoundaryA, &g_oSnowAccumBoundaryA, FavoriteNativeSource::SnowBoundaryA, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::SnowAdvanced },
    { "snow_accum_boundary_b", "Weather", "Accumulation Boundary B", -1000.0f, 1500.0f, -1000.0f, 1500.0f, "%.1f", &WeatherPresetData::snowAccumBoundaryB, &WeatherPresetData::snowAccumBoundaryBEnabled, &WeatherPresetSourceMask::snowAccumBoundaryB, &g_oSnowAccumBoundaryB, FavoriteNativeSource::SnowBoundaryB, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::SnowAdvanced },
    { "snow_coverage_threshold", "Weather", "Coverage Threshold", -1000.0f, 1500.0f, -1000.0f, 1500.0f, "%.1f", &WeatherPresetData::snowCoverageThreshold, &WeatherPresetData::snowCoverageThresholdEnabled, &WeatherPresetSourceMask::snowCoverageThreshold, &g_oSnowCoverageThreshold, FavoriteNativeSource::SnowCoverageThreshold, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::SnowAdvanced },
    { "thunder", "Weather", "Thunder", 0.0f, 1.0f, 0.0f, 5.0f, "%.3f", &WeatherPresetData::thunder, nullptr, &WeatherPresetSourceMask::thunder, &g_oThunder, FavoriteNativeSource::Zero, FavoritePresetMode::ValueOnly, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::Thunder },
    { "wind_general", "General", "Wind", 0.0f, 15.0f, 0.0f, 50.0f, "x%.2f", &WeatherPresetData::wind, nullptr, &WeatherPresetSourceMask::wind, nullptr, FavoriteNativeSource::One, FavoritePresetMode::ValueOnly, FavoriteRuntimeMode::WindMultiplier, FavoriteGate::Wind },
    { "rayleigh_height", "Atmosphere", "Rayleigh Height", 10.0f, 20000.0f, 1.0f, 200000.0f, "%.1f", &WeatherPresetData::rayleighHeight, &WeatherPresetData::rayleighHeightEnabled, &WeatherPresetSourceMask::rayleighHeight, &g_oRayleighHeight, FavoriteNativeSource::RayleighHeight, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::WindPack },
    { "ozone_ratio", "Atmosphere", "Ozone Ratio", 0.0f, 10.0f, 0.0f, 100.0f, "%.4f", &WeatherPresetData::ozoneRatio, &WeatherPresetData::ozoneRatioEnabled, &WeatherPresetSourceMask::ozoneRatio, &g_oOzoneRatio, FavoriteNativeSource::OzoneRatio, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::WindPack },
    { "cloud_amount", "Atmosphere", "Cloud Amount", 0.0f, 15.0f, 0.0f, 50.0f, "x%.2f", &WeatherPresetData::cloudAmount, &WeatherPresetData::cloudAmountEnabled, &WeatherPresetSourceMask::cloudAmount, &g_oCloudAmount, FavoriteNativeSource::One, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::Cloud },
    { "cloud_height", "Atmosphere", "Cloud Height", -15.0f, 15.0f, -50.0f, 50.0f, "x%.2f", &WeatherPresetData::cloudHeight, &WeatherPresetData::cloudHeightEnabled, &WeatherPresetSourceMask::cloudHeight, &g_oCloudSpdX, FavoriteNativeSource::One, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::Cloud },
    { "cloud_density", "Atmosphere", "Cloud Density", 0.0f, 10.0f, 0.0f, 50.0f, "x%.2f", &WeatherPresetData::cloudDensity, &WeatherPresetData::cloudDensityEnabled, &WeatherPresetSourceMask::cloudDensity, &g_oCloudSpdY, FavoriteNativeSource::One, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::Cloud },
    { "mid_clouds", "Atmosphere", "Mid Clouds", 0.0f, 15.0f, 0.0f, 50.0f, "x%.2f", &WeatherPresetData::midClouds, &WeatherPresetData::midCloudsEnabled, &WeatherPresetSourceMask::midClouds, &g_oHighClouds, FavoriteNativeSource::One, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::Cloud },
    { "high_clouds", "Atmosphere", "High Clouds", 0.0f, 15.0f, 0.0f, 50.0f, "x%.2f", &WeatherPresetData::highClouds, &WeatherPresetData::highCloudsEnabled, &WeatherPresetSourceMask::highClouds, &g_oAtmoAlpha, FavoriteNativeSource::One, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::Cloud },
    { "cloud_alpha", "Atmosphere", "Cloud Alpha", 0.0f, 50.0f, 0.0f, 100.0f, "%.3f", &WeatherPresetData::cloudAlpha, &WeatherPresetData::cloudAlphaEnabled, &WeatherPresetSourceMask::cloudAlpha, &g_oCloudAlpha, FavoriteNativeSource::CloudAlpha, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::Cloud },
    { "cloud_fade_range", "Atmosphere", "Cloud Fade Range", 0.0f, 100000.0f, 0.0f, 200000.0f, "%.1f", &WeatherPresetData::cloudFadeRange, &WeatherPresetData::cloudFadeRangeEnabled, &WeatherPresetSourceMask::cloudFadeRange, &g_oCloudFadeRange, FavoriteNativeSource::CloudFadeRange, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::Cloud },
    { "cloud_detail_ratio", "Atmosphere", "Cloud Detail Ratio", 0.0f, 1.5f, 0.0f, 1.5f, "%.4f", &WeatherPresetData::cloudDetailRatio, &WeatherPresetData::cloudDetailRatioEnabled, &WeatherPresetSourceMask::cloudDetailRatio, &g_oCloudDetailRatio, FavoriteNativeSource::CloudDetailRatio, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::Cloud },
    { "cloud_phase_front", "Atmosphere", "Cloud Phase Front", -1.0f, 1.0f, -1.0f, 1.0f, "%.4f", &WeatherPresetData::cloudPhaseFront, &WeatherPresetData::cloudPhaseFrontEnabled, &WeatherPresetSourceMask::cloudPhaseFront, &g_oCloudPhaseFront, FavoriteNativeSource::CloudPhaseFront, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::Cloud },
    { "cloud_scattering_coefficient", "Atmosphere", "Cloud Scattering Coefficient", kCloudScatteringCoefficientMin, 1.0f, kCloudScatteringCoefficientMin, 100.0f, "%.5f", &WeatherPresetData::cloudScatteringCoefficient, &WeatherPresetData::cloudScatteringCoefficientEnabled, &WeatherPresetSourceMask::cloudScatteringCoefficient, &g_oCloudScatteringCoefficient, FavoriteNativeSource::CloudScattering, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::Cloud },
    { "cloud_flow", "Atmosphere", "Cloud Flow", 0.0f, 10.0f, 0.0f, 50.0f, "x%.3f", &WeatherPresetData::cloudFlow, &WeatherPresetData::cloudFlowEnabled, &WeatherPresetSourceMask::cloudFlow, &g_oCloudFlow, FavoriteNativeSource::CloudFlow, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::Cloud },
    { "cloud_visible_range", "Atmosphere", "Cloud Visible Range", 0.0f, 10.0f, 0.0f, 10.0f, "x%.3f", &WeatherPresetData::cloudVisibleRange, &WeatherPresetData::cloudVisibleRangeEnabled, &WeatherPresetSourceMask::cloudVisibleRange, &g_oCloudVisibleRange, FavoriteNativeSource::One, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::Cloud },
    { "fog_from_wind", "Atmosphere", "Fog", 0.0f, 15.0f, 0.0f, 50.0f, "%.2f", &WeatherPresetData::nativeFog, &WeatherPresetData::nativeFogEnabled, &WeatherPresetSourceMask::nativeFog, &g_oNativeFog, FavoriteNativeSource::One, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::FogFromWind },
    { "mie_scale_height", "Atmosphere", "Aerosol Height", 10.0f, 20000.0f, 1.0f, 200000.0f, "%.1f", &WeatherPresetData::mieScaleHeight, &WeatherPresetData::mieScaleHeightEnabled, &WeatherPresetSourceMask::mieScaleHeight, &g_oMieScaleHeight, FavoriteNativeSource::AerosolHeight, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::FogFromWind },
    { "mie_aerosol_density", "Atmosphere", "Aerosol Density", 0.0f, 20.0f, 0.0f, 100.0f, "%.4f", &WeatherPresetData::mieAerosolDensity, &WeatherPresetData::mieAerosolDensityEnabled, &WeatherPresetSourceMask::mieAerosolDensity, &g_oMieAerosolDensity, FavoriteNativeSource::AerosolDensity, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::FogFromWind },
    { "mie_aerosol_absorption", "Atmosphere", "Aerosol Absorption", 0.0f, 5.0f, 0.0f, 100.0f, "%.4f", &WeatherPresetData::mieAerosolAbsorption, &WeatherPresetData::mieAerosolAbsorptionEnabled, &WeatherPresetSourceMask::mieAerosolAbsorption, &g_oMieAerosolAbsorption, FavoriteNativeSource::AerosolAbsorption, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::FogFromWind },
    { "height_fog_baseline", "Atmosphere", "Fog Height Baseline", -5000.0f, 5000.0f, -50000.0f, 50000.0f, "%.1f", &WeatherPresetData::heightFogBaseline, &WeatherPresetData::heightFogBaselineEnabled, &WeatherPresetSourceMask::heightFogBaseline, &g_oHeightFogBaseline, FavoriteNativeSource::FogBaseline, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::FogFromWind },
    { "height_fog_falloff", "Atmosphere", "Fog Height Falloff", 0.0f, 5.0f, 0.0f, 100.0f, "%.4f", &WeatherPresetData::heightFogFalloff, &WeatherPresetData::heightFogFalloffEnabled, &WeatherPresetSourceMask::heightFogFalloff, &g_oHeightFogFalloff, FavoriteNativeSource::FogFalloff, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::FogFromWind },
    { "night_sky_tilt", "Celestial", "Night Sky Tilt", -89.0f, 89.0f, -180.0f, 180.0f, "%.2f", &WeatherPresetData::nightSkyRotation, &WeatherPresetData::nightSkyRotationEnabled, &WeatherPresetSourceMask::nightSkyRotation, &g_oExpNightSkyRot, FavoriteNativeSource::NightSkyTilt, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::CelestialWind },
    { "night_sky_phase", "Celestial", "Night Sky Phase", -180.0f, 180.0f, -360.0f, 360.0f, "%.2f", &WeatherPresetData::nightSkyYaw, &WeatherPresetData::nightSkyYawEnabled, &WeatherPresetSourceMask::nightSkyYaw, &g_oNightSkyYaw, FavoriteNativeSource::NightSkyPhase, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::CelestialScene },
    { "sun_light_intensity", "Celestial", "Sun Light Intensity", 0.0f, 20.0f, 0.0f, 100.0f, "%.3f", &WeatherPresetData::sunLightIntensity, &WeatherPresetData::sunLightIntensityEnabled, &WeatherPresetSourceMask::sunLightIntensity, &g_oSunLightIntensity, FavoriteNativeSource::SunLightIntensity, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::CelestialWind },
    { "sun_size", "Celestial", "Sun Size", 0.01f, 10.0f, 0.001f, 100.0f, "%.3f", &WeatherPresetData::sunSize, &WeatherPresetData::sunSizeEnabled, &WeatherPresetSourceMask::sunSize, &g_oSunSize, FavoriteNativeSource::SunSize, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::CelestialWind },
    { "sun_yaw", "Celestial", "Sun Yaw Lock", -180.0f, 180.0f, -360.0f, 360.0f, "%.2f", &WeatherPresetData::sunYaw, &WeatherPresetData::sunYawEnabled, &WeatherPresetSourceMask::sunYaw, &g_oSunDirX, FavoriteNativeSource::SunYaw, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::CelestialScene },
    { "sun_pitch", "Celestial", "Sun Pitch Lock", -89.0f, 89.0f, -180.0f, 180.0f, "%.2f", &WeatherPresetData::sunPitch, &WeatherPresetData::sunPitchEnabled, &WeatherPresetSourceMask::sunPitch, &g_oSunDirY, FavoriteNativeSource::SunPitch, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::CelestialScene },
    { "moon_light_intensity", "Celestial", "Moon Light Intensity", 0.0f, 20.0f, 0.0f, 100.0f, "%.3f", &WeatherPresetData::moonLightIntensity, &WeatherPresetData::moonLightIntensityEnabled, &WeatherPresetSourceMask::moonLightIntensity, &g_oMoonLightIntensity, FavoriteNativeSource::MoonLightIntensity, FavoritePresetMode::EnabledAlways, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::CelestialWind },
    { "moon_size", "Celestial", "Moon Size", 0.020f, 20.0f, 0.001f, 100.0f, "%.3f", &WeatherPresetData::moonSize, &WeatherPresetData::moonSizeEnabled, &WeatherPresetSourceMask::moonSize, &g_oMoonSize, FavoriteNativeSource::MoonSize, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::CelestialWind },
    { "moon_yaw", "Celestial", "Moon Yaw Lock", -180.0f, 180.0f, -360.0f, 360.0f, "%.2f", &WeatherPresetData::moonYaw, &WeatherPresetData::moonYawEnabled, &WeatherPresetSourceMask::moonYaw, &g_oMoonDirX, FavoriteNativeSource::MoonYaw, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::CelestialScene },
    { "moon_pitch", "Celestial", "Moon Pitch Lock", -89.0f, 89.0f, -180.0f, 180.0f, "%.2f", &WeatherPresetData::moonPitch, &WeatherPresetData::moonPitchEnabled, &WeatherPresetSourceMask::moonPitch, &g_oMoonDirY, FavoriteNativeSource::MoonPitch, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::CelestialScene },
    { "moon_roll", "Celestial", "Moon Rotation", -180.0f, 180.0f, -360.0f, 360.0f, "%.2f", &WeatherPresetData::moonRoll, &WeatherPresetData::moonRollEnabled, &WeatherPresetSourceMask::moonRoll, &g_oMoonRoll, FavoriteNativeSource::MoonRotation, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::CelestialScene },
    { "2c", "Experiment", "2C", 0.0f, 15.0f, 0.0f, 50.0f, "x%.2f", &WeatherPresetData::exp2C, &WeatherPresetData::exp2CEnabled, &WeatherPresetSourceMask::exp2C, &g_oExpCloud2C, FavoriteNativeSource::One, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::Experiment },
    { "2d", "Experiment", "2D", 0.0f, 15.0f, 0.0f, 50.0f, "x%.2f", &WeatherPresetData::exp2D, &WeatherPresetData::exp2DEnabled, &WeatherPresetSourceMask::exp2D, &g_oExpCloud2D, FavoriteNativeSource::One, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::Experiment },
    { "cloud_variation", "Experiment", "Cloud Variation [32]", 0.0f, 15.0f, 0.0f, 50.0f, "x%.2f", &WeatherPresetData::cloudVariation, &WeatherPresetData::cloudVariationEnabled, &WeatherPresetSourceMask::cloudVariation, &g_oCloudVariation, FavoriteNativeSource::One, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideClearAtNative, FavoriteGate::Experiment },
    { "fog_legacy", "Experiment", "Fog [LEGACY]", 0.0f, 100.0f, 0.0f, 500.0f, "%.1f%%", &WeatherPresetData::fogPercent, &WeatherPresetData::fogEnabled, &WeatherPresetSourceMask::fog, &g_oFog, FavoriteNativeSource::Zero, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::LegacyFog, FavoriteGate::LegacyFog },
    { "puddle", "Experiment", "Puddle Scale", 0.0f, 1.0f, 0.0f, 5.0f, "%.3f", &WeatherPresetData::puddleScale, &WeatherPresetData::puddleScaleEnabled, &WeatherPresetSourceMask::puddleScale, &g_oCloudThk, FavoriteNativeSource::Zero, FavoritePresetMode::EnabledAwayFromNative, FavoriteRuntimeMode::OverrideAlways, FavoriteGate::Detail },
};

const FavoriteExtraSpec kExtraSpecs[] = {
    { "force_clear", "Weather", "Force Clear Sky", FavoriteExtraKind::ForceClear },
    { "no_rain", "Weather", "No Rain", FavoriteExtraKind::NoRain },
    { "no_dust", "Weather", "No Dust", FavoriteExtraKind::NoDust },
    { "no_snow", "Weather", "No Snow", FavoriteExtraKind::NoSnow },
    { "time_controls", "General", "Time Controls", FavoriteExtraKind::TimeControls },
    { "no_wind", "General", "No Wind", FavoriteExtraKind::NoWind },
    { "rayleigh_scattering_color", "Atmosphere", "Rayleigh Scattering Color", FavoriteExtraKind::RayleighColor },
    { "no_fog", "Atmosphere", "No Fog", FavoriteExtraKind::NoFog },
    { "volume_fog_scatter_color", "Atmosphere", "Volume Fog Scatter Color", FavoriteExtraKind::VolumeFogColor },
    { "mie_scatter_color", "Atmosphere", "Mie Scatter Color", FavoriteExtraKind::MieColor },
    { "milkyway_texture", "Celestial", "Milky Way Texture", FavoriteExtraKind::MilkywayTexture },
    { "moon_texture", "Celestial", "Moon Texture", FavoriteExtraKind::MoonTexture },
};

const FavoriteSliderSpec* FindSliderSpec(const std::string& id) {
    for (const FavoriteSliderSpec& spec : kSliderSpecs) {
        if (id == spec.id) {
            return &spec;
        }
    }
    return nullptr;
}

const FavoriteExtraSpec* FindExtraSpec(const std::string& id) {
    for (const FavoriteExtraSpec& spec : kExtraSpecs) {
        if (id == spec.id) {
            return &spec;
        }
    }
    return nullptr;
}

bool IsRegisteredControl(const std::string& id) {
    return FindSliderSpec(id) || FindExtraSpec(id);
}

std::string NormalizeControlId(const char* id) {
    if (strcmp(id, "visual_time_controls") == 0 || strcmp(id, "general_controls") == 0) {
        return "time_controls";
    }
    return id;
}

const char* ControlLabel(const std::string& id) {
    if (const FavoriteSliderSpec* slider = FindSliderSpec(id)) {
        return slider->label;
    }
    if (const FavoriteExtraSpec* extra = FindExtraSpec(id)) {
        return extra->label;
    }
    return "";
}

const char* ControlCategory(const std::string& id) {
    if (const FavoriteSliderSpec* slider = FindSliderSpec(id)) {
        return slider->category;
    }
    if (const FavoriteExtraSpec* extra = FindExtraSpec(id)) {
        return extra->category;
    }
    return "";
}

float NativeValue(FavoriteNativeSource source) {
    switch (source) {
    case FavoriteNativeSource::Zero: return 0.0f;
    case FavoriteNativeSource::One: return 1.0f;
    case FavoriteNativeSource::SnowBoundaryA: return -5.0f;
    case FavoriteNativeSource::SnowBoundaryB: return -20.0f;
    case FavoriteNativeSource::SnowCoverageThreshold: return -20.0f;
    case FavoriteNativeSource::RayleighHeight: return g_windPackBase0E.load();
    case FavoriteNativeSource::OzoneRatio: return g_windPackBase14.load();
    case FavoriteNativeSource::CloudAlpha: return g_windPackBase1E.load();
    case FavoriteNativeSource::CloudFadeRange: return g_windPackBase27.load();
    case FavoriteNativeSource::CloudDetailRatio: return g_windPackBase28.load();
    case FavoriteNativeSource::CloudPhaseFront: return g_windPackBase21.load();
    case FavoriteNativeSource::CloudScattering: return g_windPackBase20.load();
    case FavoriteNativeSource::CloudFlow: return g_windPackBase1F.load();
    case FavoriteNativeSource::AerosolHeight: return g_windPackBase10.load();
    case FavoriteNativeSource::AerosolDensity: return g_windPackBase11.load();
    case FavoriteNativeSource::AerosolAbsorption: return g_windPackBase12.load();
    case FavoriteNativeSource::FogBaseline: return g_windPackBase18.load();
    case FavoriteNativeSource::FogFalloff: return g_windPackBase19.load();
    case FavoriteNativeSource::NightSkyTilt:
        return (g_windPackBase0AValid.load() && g_windPackBase0BValid.load())
            ? min(89.0f, max(-89.0f, g_windPackBase0A.load() + 90.0f - g_windPackBase0B.load()))
            : 0.0f;
    case FavoriteNativeSource::NightSkyPhase: return g_sceneBaseNightSkyYaw.load();
    case FavoriteNativeSource::SunLightIntensity: return g_windPackBase00.load();
    case FavoriteNativeSource::SunSize: return g_atmoBaseSunSize.load();
    case FavoriteNativeSource::SunYaw: return g_sceneBaseSunYaw.load();
    case FavoriteNativeSource::SunPitch: return g_sceneBaseSunPitch.load();
    case FavoriteNativeSource::MoonLightIntensity: return g_windPackBase05.load();
    case FavoriteNativeSource::MoonSize: return g_atmoBaseMoonSize.load();
    case FavoriteNativeSource::MoonYaw: return g_sceneBaseMoonYaw.load();
    case FavoriteNativeSource::MoonPitch: return g_sceneBaseMoonPitch.load();
    case FavoriteNativeSource::MoonRotation: return 0.0f;
    }
    return 0.0f;
}

bool IsGateEnabled(FavoriteGate gate, const WeatherPresetData& data, bool detachedEdit) {
    const bool forceClear = detachedEdit ? data.forceClearSky : g_forceClear.load();
    const bool noRain = detachedEdit ? data.noRain : g_noRain.load();
    const bool noDust = detachedEdit ? data.noDust : g_noDust.load();
    const bool noSnow = detachedEdit ? data.noSnow : g_noSnow.load();
    const bool noWind = detachedEdit ? data.noWind : g_noWind.load();
    const bool noFog = detachedEdit ? data.noFog : g_noFog.load();
    switch (gate) {
    case FavoriteGate::Rain:
        return !forceClear && !noRain && RuntimeFeatureAvailable(RuntimeFeatureId::Rain) && RainHookReady();
    case FavoriteGate::Dust:
        return !forceClear && !noDust && RuntimeFeatureAvailable(RuntimeFeatureId::Dust) && DustHookReady();
    case FavoriteGate::Snow:
        return !forceClear && !noSnow && RuntimeFeatureAvailable(RuntimeFeatureId::Snow) && SnowHookReady();
    case FavoriteGate::SnowAdvanced:
        return !forceClear && !noSnow && WeatherTickReady();
    case FavoriteGate::Thunder:
        return !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::ThunderControls) && WeatherTickReady();
    case FavoriteGate::Wind:
        return !noWind && RuntimeFeatureAvailable(RuntimeFeatureId::WindControls) && WindPackReady();
    case FavoriteGate::WindPack:
        return RuntimeFeatureAvailable(RuntimeFeatureId::CloudControls) && WindPackReady();
    case FavoriteGate::Cloud:
        return !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::CloudControls) && WindPackReady();
    case FavoriteGate::FogFromWind:
        return !forceClear && !noFog && RuntimeFeatureAvailable(RuntimeFeatureId::WindControls) && WindPackReady();
    case FavoriteGate::CelestialWind:
        return RuntimeFeatureAvailable(RuntimeFeatureId::CelestialControls) && WindPackReady();
    case FavoriteGate::CelestialScene:
        return RuntimeFeatureAvailable(RuntimeFeatureId::CelestialControls) && SceneFrameReady();
    case FavoriteGate::Experiment:
        return !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::ExperimentControls) && WindPackReady();
    case FavoriteGate::LegacyFog:
        return !forceClear && !noFog && RuntimeFeatureAvailable(RuntimeFeatureId::FogControls) && WeatherFrameReady();
    case FavoriteGate::Detail:
        return RuntimeFeatureAvailable(RuntimeFeatureId::DetailControls) && WeatherTickReady();
    }
    return false;
}

bool IsControlUsed(const std::vector<FavoriteSection>& sections, const char* id) {
    for (const FavoriteSection& section : sections) {
        for (const std::string& controlId : section.controlIds) {
            if (controlId == id) {
                return true;
            }
        }
    }
    return false;
}

const char* FindControlSectionName(const std::vector<FavoriteSection>& sections, const char* id) {
    for (const FavoriteSection& section : sections) {
        for (const std::string& controlId : section.controlIds) {
            if (controlId == id) {
                return section.name[0] ? section.name : "New Section";
            }
        }
    }
    return nullptr;
}

void SaveFavoritesLayout() {
    char path[MAX_PATH] = {};
    BuildIniPath(path, sizeof(path));
    WritePrivateProfileStringA("Favorites", nullptr, nullptr, path);
    char value[32] = {};
    sprintf_s(value, "%u", static_cast<unsigned int>(g_sections.size()));
    WritePrivateProfileStringA("Favorites", "SectionCount", value, path);
    for (size_t index = 0; index < g_sections.size(); ++index) {
        char key[64] = {};
        sprintf_s(key, "Section%uName", static_cast<unsigned int>(index));
        WritePrivateProfileStringA("Favorites", key, g_sections[index].name, path);
        std::string controls;
        for (const std::string& id : g_sections[index].controlIds) {
            if (!controls.empty()) controls += ",";
            controls += id;
        }
        sprintf_s(key, "Section%uControls", static_cast<unsigned int>(index));
        WritePrivateProfileStringA("Favorites", key, controls.c_str(), path);
    }
}

void LoadFavoritesLayout() {
    if (g_layoutLoaded) {
        return;
    }
    g_layoutLoaded = true;
    char path[MAX_PATH] = {};
    BuildIniPath(path, sizeof(path));
    char value[2048] = {};
    GetPrivateProfileStringA("Favorites", "SectionCount", "0", value, sizeof(value), path);
    const int sectionCount = min(kMaxFavoriteSections, max(0, atoi(value)));
    for (int sectionIndex = 0; sectionIndex < sectionCount; ++sectionIndex) {
        FavoriteSection section;
        char key[64] = {};
        sprintf_s(key, "Section%dName", sectionIndex);
        GetPrivateProfileStringA("Favorites", key, "New Section", section.name, sizeof(section.name), path);
        sprintf_s(key, "Section%dControls", sectionIndex);
        GetPrivateProfileStringA("Favorites", key, "", value, sizeof(value), path);
        char controlList[2048] = {};
        strcpy_s(controlList, value);
        char* context = nullptr;
        int controlCount = 0;
        for (char* token = strtok_s(controlList, ",", &context);
             token && controlCount < kMaxFavoriteControlsPerSection;
            token = strtok_s(nullptr, ",", &context)) {
            const std::string controlId = NormalizeControlId(token);
            if (IsRegisteredControl(controlId) &&
                !IsControlUsed(g_sections, controlId.c_str()) &&
                std::find(section.controlIds.begin(), section.controlIds.end(), controlId) == section.controlIds.end()) {
                section.controlIds.emplace_back(controlId);
                ++controlCount;
            }
        }
        g_sections.push_back(section);
    }
}

void CommitPresetEdit(const WeatherPresetData& data, const WeatherPresetSourceMask& mask, bool regionScoped) {
    if (regionScoped) {
        Preset_SetEditRegionDataWithOverrides(data, mask);
    } else {
        Preset_SetEditRegionData(data);
    }
}

void DrawFavoriteSlider(
    const FavoriteSliderSpec& spec,
    WeatherPresetData* providedEditData,
    WeatherPresetSourceMask* providedOverrideMask,
    bool* providedEditChanged) {
    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData localEditData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    WeatherPresetData& editData = providedEditData ? *providedEditData : localEditData;
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask localOverrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    WeatherPresetSourceMask& overrideMask = providedOverrideMask ? *providedOverrideMask : localOverrideMask;
    bool localEditChanged = false;
    bool& editChanged = providedEditChanged ? *providedEditChanged : localEditChanged;
    const float nativeValue = NativeValue(spec.nativeSource);
    bool nativeDisplay = false;
    float value = nativeValue;
    if (detachedEdit) {
        if (spec.enabledField && !(editData.*(spec.enabledField))) {
            value = nativeValue;
            nativeDisplay = true;
        } else {
            value = editData.*(spec.valueField);
            nativeDisplay = spec.presetMode == FavoritePresetMode::ValueOnly &&
                fabsf(value - nativeValue) <= kNativeEpsilon;
        }
    } else if (spec.runtimeMode == FavoriteRuntimeMode::WindMultiplier) {
        value = g_windMul.load();
        nativeDisplay = fabsf(value - nativeValue) <= kNativeEpsilon;
    } else if (spec.runtimeMode == FavoriteRuntimeMode::LegacyFog) {
        nativeDisplay = !spec.runtimeValue->active.load();
        if (!nativeDisplay) {
            value = sqrtf(max(0.0f, spec.runtimeValue->value.load() / 100.0f)) * 100.0f;
        }
    } else {
        nativeDisplay = !spec.runtimeValue->active.load();
        value = nativeDisplay ? nativeValue : spec.runtimeValue->value.load();
    }

    const SliderRange range = ActiveSliderRange(spec.normalLo, spec.normalHi, spec.extendedLo, spec.extendedHi);
    const bool enabled = IsGateEnabled(spec.gate, editData, detachedEdit);
    bool valueChanged = false;
    bool overrideChanged = false;
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    const bool reset = DrawSliderFloatRow(
        spec.label,
        spec.id,
        &value,
        range.lo,
        range.hi,
        spec.format,
        &valueChanged,
        regionScoped ? &(overrideMask.*(spec.maskField)) : nullptr,
        &overrideChanged,
        nativeDisplay);
    if (!enabled) {
        ImGui::EndDisabled();
    }

    if (overrideChanged) {
        editChanged = true;
    }
    if (reset) {
        if (detachedEdit) {
            editData.*(spec.valueField) = nativeValue;
            if (spec.enabledField) editData.*(spec.enabledField) = false;
            if (regionScoped) overrideMask.*(spec.maskField) = true;
            editChanged = true;
        } else if (spec.runtimeMode == FavoriteRuntimeMode::WindMultiplier) {
            g_windMul.store(nativeValue);
        } else {
            spec.runtimeValue->clear();
            if (spec.gate == FavoriteGate::SnowAdvanced) {
                g_snowCoverageGlobalsDirty.store(true);
            }
        }
    } else if (enabled && valueChanged) {
        value = ClampSliderValue(value, range);
        if (detachedEdit) {
            editData.*(spec.valueField) = value;
            if (spec.enabledField) {
                editData.*(spec.enabledField) =
                    spec.presetMode == FavoritePresetMode::EnabledAwayFromNative
                        ? fabsf(value - nativeValue) > kNativeEpsilon
                        : true;
            }
            if (regionScoped) overrideMask.*(spec.maskField) = true;
            editChanged = true;
        } else if (spec.runtimeMode == FavoriteRuntimeMode::WindMultiplier) {
            g_windMul.store(value);
        } else if (spec.runtimeMode == FavoriteRuntimeMode::LegacyFog) {
            const float normalized = value * 0.01f;
            spec.runtimeValue->set(normalized * normalized * 100.0f);
        } else if (spec.runtimeMode == FavoriteRuntimeMode::OverrideClearAtNative &&
                   fabsf(value - nativeValue) <= kNativeEpsilon) {
            spec.runtimeValue->clear();
        } else {
            spec.runtimeValue->set(value);
            if (spec.gate == FavoriteGate::SnowAdvanced) {
                g_snowCoverageGlobalsDirty.store(true);
            }
        }
    }
    if (detachedEdit && !providedEditData && editChanged) {
        CommitPresetEdit(editData, overrideMask, regionScoped);
    }
}

void DrawFavoriteToggle(
    FavoriteExtraKind kind,
    WeatherPresetData* providedEditData,
    WeatherPresetSourceMask* providedOverrideMask,
    bool* providedEditChanged) {
    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData localEditData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    WeatherPresetData& editData = providedEditData ? *providedEditData : localEditData;
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask localOverrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    WeatherPresetSourceMask& overrideMask = providedOverrideMask ? *providedOverrideMask : localOverrideMask;
    bool localEditChanged = false;
    bool& editChanged = providedEditChanged ? *providedEditChanged : localEditChanged;

    const char* label = nullptr;
    const char* id = nullptr;
    const char* enabledStatus = nullptr;
    const char* disabledStatus = nullptr;
    bool WeatherPresetData::* valueField = nullptr;
    bool WeatherPresetSourceMask::* maskField = nullptr;
    std::atomic<bool>* runtimeValue = nullptr;
    bool enabled = true;
    RuntimeFeatureId unavailableFeature = RuntimeFeatureId::ForceClear;
    RuntimeHookId unavailableHook = RuntimeHookId::WeatherTick;
    bool showUnavailableFeature = false;
    bool showUnavailableHook = false;
    const bool forceClear = detachedEdit ? editData.forceClearSky : g_forceClear.load();

    switch (kind) {
    case FavoriteExtraKind::ForceClear:
        label = "Force Clear Sky"; id = "force_clear";
        valueField = &WeatherPresetData::forceClearSky; maskField = &WeatherPresetSourceMask::forceClearSky;
        runtimeValue = &g_forceClear; enabledStatus = "Force clear active"; disabledStatus = "Force clear off";
        enabled = RuntimeFeatureAvailable(RuntimeFeatureId::ForceClear) && WeatherTickReady();
        unavailableFeature = RuntimeFeatureId::ForceClear;
        showUnavailableFeature = !RuntimeFeatureAvailable(RuntimeFeatureId::ForceClear);
        showUnavailableHook = !showUnavailableFeature;
        break;
    case FavoriteExtraKind::NoRain:
        label = "No Rain"; id = "no_rain";
        valueField = &WeatherPresetData::noRain; maskField = &WeatherPresetSourceMask::noRain;
        runtimeValue = &g_noRain; enabledStatus = "No Rain enabled"; disabledStatus = "No Rain disabled";
        enabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Rain) && RainHookReady();
        unavailableHook = RuntimeHookId::GetRainIntensity;
        showUnavailableHook = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Rain);
        break;
    case FavoriteExtraKind::NoDust:
        label = "No Dust"; id = "no_dust";
        valueField = &WeatherPresetData::noDust; maskField = &WeatherPresetSourceMask::noDust;
        runtimeValue = &g_noDust; enabledStatus = "No Dust enabled"; disabledStatus = "No Dust disabled";
        enabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Dust) && DustHookReady();
        unavailableHook = RuntimeHookId::GetDustIntensity;
        showUnavailableHook = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Dust);
        break;
    case FavoriteExtraKind::NoSnow:
        label = "No Snow"; id = "no_snow";
        valueField = &WeatherPresetData::noSnow; maskField = &WeatherPresetSourceMask::noSnow;
        runtimeValue = &g_noSnow; enabledStatus = "No Snow enabled"; disabledStatus = "No Snow disabled";
        enabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Snow) && SnowHookReady();
        unavailableHook = RuntimeHookId::GetSnowIntensity;
        showUnavailableHook = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Snow);
        break;
    case FavoriteExtraKind::NoWind:
        label = "No Wind"; id = "no_wind";
        valueField = &WeatherPresetData::noWind; maskField = &WeatherPresetSourceMask::noWind;
        runtimeValue = &g_noWind; enabledStatus = "No Wind enabled"; disabledStatus = "No Wind disabled";
        enabled = RuntimeFeatureAvailable(RuntimeFeatureId::NoWindControls) && WeatherTickReady();
        unavailableFeature = RuntimeFeatureId::NoWindControls;
        showUnavailableFeature = !RuntimeFeatureAvailable(RuntimeFeatureId::NoWindControls);
        showUnavailableHook = !showUnavailableFeature;
        break;
    case FavoriteExtraKind::NoFog:
        label = "No Fog"; id = "no_fog";
        valueField = &WeatherPresetData::noFog; maskField = &WeatherPresetSourceMask::noFog;
        runtimeValue = &g_noFog; enabledStatus = "No Fog enabled"; disabledStatus = "No Fog disabled";
        enabled = (RuntimeFeatureAvailable(RuntimeFeatureId::FogControls) && WeatherFrameReady()) ||
            (RuntimeFeatureAvailable(RuntimeFeatureId::WindControls) && WindPackReady());
        unavailableFeature = RuntimeFeatureId::FogControls;
        unavailableHook = RuntimeHookId::WeatherFrameUpdate;
        showUnavailableFeature = !RuntimeFeatureAvailable(RuntimeFeatureId::FogControls) &&
            !RuntimeFeatureAvailable(RuntimeFeatureId::WindControls);
        showUnavailableHook = !showUnavailableFeature;
        break;
    default:
        return;
    }

    bool value = detachedEdit ? editData.*valueField : runtimeValue->load();
    if (!enabled) ImGui::BeginDisabled();
    bool overrideChanged = false;
    const bool changed = regionScoped
        ? DrawOverrideCheckboxRow(label, id, &value, &(overrideMask.*maskField), &overrideChanged)
        : ImGui::Checkbox(label, &value);
    if (!enabled) ImGui::EndDisabled();
    if (overrideChanged) {
        editChanged = true;
    }
    if (changed) {
        if (detachedEdit) {
            editData.*valueField = value;
            if (regionScoped) overrideMask.*maskField = true;
            editChanged = true;
        } else {
            runtimeValue->store(value);
        }
        GUI_SetStatus(value ? enabledStatus : disabledStatus);
    }
    if (!enabled) {
        if (showUnavailableFeature) {
            DrawFeatureUnavailable(unavailableFeature);
        } else if (showUnavailableHook) {
            DrawHookUnavailable(unavailableHook);
        }
    }
    if (detachedEdit && !providedEditData && editChanged) {
        CommitPresetEdit(editData, overrideMask, regionScoped);
    }
}

void DrawFavoriteColor(
    FavoriteExtraKind kind,
    WeatherPresetData* providedEditData,
    WeatherPresetSourceMask* providedOverrideMask,
    bool* providedEditChanged) {
    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData localEditData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    WeatherPresetData& editData = providedEditData ? *providedEditData : localEditData;
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask localOverrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    WeatherPresetSourceMask& overrideMask = providedOverrideMask ? *providedOverrideMask : localOverrideMask;
    bool localEditChanged = false;
    bool& editChanged = providedEditChanged ? *providedEditChanged : localEditChanged;

    const char* label = nullptr;
    const char* id = nullptr;
    WeatherPresetColor native{};
    WeatherPresetColor WeatherPresetData::* valueField = nullptr;
    bool WeatherPresetData::* enabledField = nullptr;
    bool WeatherPresetSourceMask::* maskField = nullptr;
    ColorOverride* runtimeValue = nullptr;
    bool includeAlpha = true;
    bool enabled = true;
    if (kind == FavoriteExtraKind::RayleighColor) {
        label = "Rayleigh Scattering Color"; id = "rayleigh_scattering_color";
        native = RayleighColorFromUiBits(g_windPackBase0FBits.load());
        valueField = &WeatherPresetData::rayleighScatteringColor;
        enabledField = &WeatherPresetData::rayleighScatteringColorEnabled;
        maskField = &WeatherPresetSourceMask::rayleighScatteringColor;
        runtimeValue = &g_oRayleighScatteringColor;
        includeAlpha = false;
        enabled = RuntimeFeatureAvailable(RuntimeFeatureId::CloudControls) && WindPackReady();
    } else if (kind == FavoriteExtraKind::VolumeFogColor) {
        label = "Volume Fog Scatter Color"; id = "volume_fog_scatter_color";
        native = { g_windPackBase34.load(), g_windPackBase35.load(), g_windPackBase36.load(), g_windPackBase37.load() };
        valueField = &WeatherPresetData::volumeFogScatterColor;
        enabledField = &WeatherPresetData::volumeFogScatterColorEnabled;
        maskField = &WeatherPresetSourceMask::volumeFogScatterColor;
        runtimeValue = &g_oVolumeFogScatterColor;
        enabled = !(detachedEdit ? (editData.forceClearSky || editData.noFog) : (g_forceClear.load() || g_noFog.load())) &&
            RuntimeFeatureAvailable(RuntimeFeatureId::WindControls) && WindPackReady();
    } else if (kind == FavoriteExtraKind::MieColor) {
        label = "Mie Scatter Color"; id = "mie_scatter_color";
        native = { g_windPackBase38.load(), g_windPackBase39.load(), g_windPackBase3A.load(), g_windPackBase3B.load() };
        valueField = &WeatherPresetData::mieScatterColor;
        enabledField = &WeatherPresetData::mieScatterColorEnabled;
        maskField = &WeatherPresetSourceMask::mieScatterColor;
        runtimeValue = &g_oMieScatterColor;
        enabled = !(detachedEdit ? (editData.forceClearSky || editData.noFog) : (g_forceClear.load() || g_noFog.load())) &&
            RuntimeFeatureAvailable(RuntimeFeatureId::WindControls) && WindPackReady();
    } else {
        return;
    }
    WeatherPresetColor colorData = detachedEdit
        ? ((editData.*enabledField) ? editData.*valueField : native)
        : (runtimeValue->active.load()
            ? WeatherPresetColor{ runtimeValue->r.load(), runtimeValue->g.load(), runtimeValue->b.load(), runtimeValue->a.load() }
            : native);
    float color[4] = { colorData.r, colorData.g, colorData.b, colorData.a };
    const bool nativeDisplay = detachedEdit ? !(editData.*enabledField) : !runtimeValue->active.load();
    bool changed = false;
    bool overrideChanged = false;
    if (!enabled) ImGui::BeginDisabled();
    const bool reset = DrawColorRow(label, id, color, includeAlpha, &changed,
        regionScoped ? &(overrideMask.*maskField) : nullptr, &overrideChanged, nativeDisplay);
    if (!enabled) ImGui::EndDisabled();
    if (reset) {
        if (detachedEdit) {
            editData.*enabledField = false;
            editData.*valueField = native;
            if (regionScoped) overrideMask.*maskField = true;
            editChanged = true;
        } else {
            runtimeValue->clear();
        }
    } else if (overrideChanged) {
        editChanged = true;
    } else if (enabled && changed) {
        ClampColorValues(color, includeAlpha);
        if (detachedEdit) {
            editData.*valueField = ColorFromArray(color, includeAlpha);
            editData.*enabledField = true;
            if (regionScoped) overrideMask.*maskField = true;
            editChanged = true;
        } else {
            runtimeValue->set(color[0], color[1], color[2], includeAlpha ? color[3] : 1.0f);
        }
    }
    if (detachedEdit && !providedEditData && editChanged) {
        CommitPresetEdit(editData, overrideMask, regionScoped);
    }
}

void DrawFavoriteMilkywayTexture(
    WeatherPresetData* providedEditData,
    WeatherPresetSourceMask* providedOverrideMask,
    bool* providedEditChanged) {
    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData localEditData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    WeatherPresetData& editData = providedEditData ? *providedEditData : localEditData;
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask localOverrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    WeatherPresetSourceMask& overrideMask = providedOverrideMask ? *providedOverrideMask : localOverrideMask;
    bool localEditChanged = false;
    bool& editChanged = providedEditChanged ? *providedEditChanged : localEditChanged;
    const bool celestialEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::CelestialControls);
    if (!celestialEnabled) ImGui::BeginDisabled();
    int selected = detachedEdit
        ? (editData.milkywayTextureEnabled ? MilkywayTextureFindOptionByName(editData.milkywayTexture.c_str()) : 0)
        : MilkywayTextureSelectedOption();
    if (selected < 0) selected = 0;
    const bool ready = MilkywayTextureReady();
    const bool overrideChanged = DrawOverrideToggle(regionScoped ? &overrideMask.milkywayTexture : nullptr);
    const bool regionOverride = !regionScoped || overrideMask.milkywayTexture;
    if (!regionOverride) ImGui::BeginDisabled();
    ImGui::TextUnformatted("Milky Way Texture");
    if (!regionOverride) ImGui::EndDisabled();
    if (regionScoped) DrawOverrideBadge(overrideMask.milkywayTexture);
    ImGui::SameLine();
    const float refreshWidth = 64.0f;
    const float refreshX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - refreshWidth;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), refreshX));
    if (ImGui::Button("Refresh##milkyway", ImVec2(refreshWidth, 0.0f))) {
        MilkywayTextureRefreshList();
    }
    char inherited[192] = {};
    const char* preview = MilkywayTextureOptionName(selected);
    if (!regionOverride) {
        sprintf_s(inherited, sizeof(inherited), "G: %s", preview);
        preview = inherited;
    }
    ImGui::TextDisabled("Selected: %s", preview);
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("Browse Milky Way Textures##milkyway_texture_browser_fav")) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##milkyway_texture_filter", "Search...", g_favoriteMilkywayTextureFilter, IM_ARRAYSIZE(g_favoriteMilkywayTextureFilter));
        if (!ready || !regionOverride) ImGui::BeginDisabled();
        const float listHeight = min(220.0f, max(112.0f, ImGui::GetTextLineHeightWithSpacing() * 9.0f));
        if (ImGui::BeginChild("MilkywayTextureLibrary", ImVec2(0.0f, listHeight), true)) {
            const char* currentPack = nullptr;
            int visibleCount = 0;
            for (int i = 0; i < MilkywayTextureOptionCount(); ++i) {
                const char* optionName = MilkywayTextureOptionName(i);
                const char* optionLabel = MilkywayTextureOptionLabel(i);
                const bool visible = i == 0
                    ? TextContainsNoCase("Native", g_favoriteMilkywayTextureFilter)
                    : (TextContainsNoCase(optionLabel, g_favoriteMilkywayTextureFilter) ||
                       TextContainsNoCase(optionName, g_favoriteMilkywayTextureFilter) ||
                       TextContainsNoCase(MilkywayTextureOptionPack(i), g_favoriteMilkywayTextureFilter));
                if (!visible) continue;
                const char* optionPack = i > 0 ? MilkywayTextureOptionPack(i) : "";
                const bool hasPack = optionPack && optionPack[0];
                if (hasPack && (!currentPack || strcmp(currentPack, optionPack) != 0)) {
                    DrawTexturePackHeader(optionPack);
                    currentPack = optionPack;
                }
                ++visibleCount;
                if (hasPack) ImGui::Indent(12.0f);
                if (ImGui::Selectable(optionLabel, i == selected)) {
                    selected = i;
                    if (detachedEdit) {
                        editData.milkywayTextureEnabled = i > 0;
                        editData.milkywayTexture = i > 0 ? optionName : "";
                        if (regionScoped) overrideMask.milkywayTexture = true;
                        editChanged = true;
                    } else {
                        MilkywayTextureSelectOption(i);
                    }
                }
                if (hasPack) ImGui::Unindent(12.0f);
                if (i == selected) ImGui::SetItemDefaultFocus();
            }
            if (visibleCount == 0) ImGui::TextDisabled("No Milky Way texture matches");
        }
        ImGui::EndChild();
        if (!ready || !regionOverride) ImGui::EndDisabled();
    }
    if (overrideChanged) editChanged = true;
    if (!celestialEnabled) ImGui::EndDisabled();
    if (detachedEdit && !providedEditData && editChanged) {
        CommitPresetEdit(editData, overrideMask, regionScoped);
    }
}

void DrawFavoriteMoonTexture(
    WeatherPresetData* providedEditData,
    WeatherPresetSourceMask* providedOverrideMask,
    bool* providedEditChanged) {
    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData localEditData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    WeatherPresetData& editData = providedEditData ? *providedEditData : localEditData;
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask localOverrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    WeatherPresetSourceMask& overrideMask = providedOverrideMask ? *providedOverrideMask : localOverrideMask;
    bool localEditChanged = false;
    bool& editChanged = providedEditChanged ? *providedEditChanged : localEditChanged;
    const bool celestialEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::CelestialControls);
    if (!celestialEnabled) ImGui::BeginDisabled();
    int selected = detachedEdit
        ? (editData.moonTextureEnabled ? MoonTextureFindOptionByName(editData.moonTexture.c_str()) : 0)
        : MoonTextureSelectedOption();
    if (selected < 0) selected = 0;
    const bool ready = MoonTextureReady();
    const bool overrideChanged = DrawOverrideToggle(regionScoped ? &overrideMask.moonTexture : nullptr);
    const bool regionOverride = !regionScoped || overrideMask.moonTexture;
    if (!regionOverride) ImGui::BeginDisabled();
    ImGui::TextUnformatted("Moon Texture");
    if (!regionOverride) ImGui::EndDisabled();
    if (regionScoped) DrawOverrideBadge(overrideMask.moonTexture);
    ImGui::SameLine();
    const float refreshWidth = 64.0f;
    const float refreshX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - refreshWidth;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), refreshX));
    if (ImGui::Button("Refresh##moon", ImVec2(refreshWidth, 0.0f))) {
        MoonTextureRefreshList();
    }
    char inherited[192] = {};
    const char* preview = MoonTextureOptionName(selected);
    if (!regionOverride) {
        sprintf_s(inherited, sizeof(inherited), "G: %s", preview);
        preview = inherited;
    }
    ImGui::TextDisabled("Selected: %s", preview);
    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("Browse Moon Textures##moon_texture_browser_fav")) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##moon_texture_filter", "Search...", g_favoriteMoonTextureFilter, IM_ARRAYSIZE(g_favoriteMoonTextureFilter));
        if (!ready || !regionOverride) ImGui::BeginDisabled();
        const float listHeight = min(220.0f, max(112.0f, ImGui::GetTextLineHeightWithSpacing() * 9.0f));
        if (ImGui::BeginChild("MoonTextureLibrary", ImVec2(0.0f, listHeight), true)) {
            const char* currentPack = nullptr;
            int visibleCount = 0;
            for (int i = 0; i < MoonTextureOptionCount(); ++i) {
                const char* optionName = MoonTextureOptionName(i);
                const char* optionLabel = MoonTextureOptionLabel(i);
                const bool animated = i > 0 && MoonTextureOptionIsAnimated(i);
                const bool visible = i == 0
                    ? TextContainsNoCase("Native", g_favoriteMoonTextureFilter)
                    : (TextContainsNoCase(optionLabel, g_favoriteMoonTextureFilter) ||
                       TextContainsNoCase(optionName, g_favoriteMoonTextureFilter) ||
                       TextContainsNoCase(MoonTextureOptionPack(i), g_favoriteMoonTextureFilter) ||
                       (animated && TextContainsNoCase("anim", g_favoriteMoonTextureFilter)));
                if (!visible) continue;
                const char* optionPack = i > 0 ? MoonTextureOptionPack(i) : "";
                const bool hasPack = optionPack && optionPack[0];
                if (hasPack && (!currentPack || strcmp(currentPack, optionPack) != 0)) {
                    DrawTexturePackHeader(optionPack);
                    currentPack = optionPack;
                }
                ++visibleCount;
                if (hasPack) ImGui::Indent(12.0f);
                char display[256] = {};
                sprintf_s(display, sizeof(display), "%s%s", optionLabel, animated ? " [ANIM]" : "");
                if (ImGui::Selectable(display, i == selected)) {
                    selected = i;
                    if (detachedEdit) {
                        editData.moonTextureEnabled = i > 0;
                        editData.moonTexture = i > 0 ? optionName : "";
                        if (regionScoped) overrideMask.moonTexture = true;
                        editChanged = true;
                    } else {
                        MoonTextureSelectOption(i);
                    }
                }
                if (hasPack) ImGui::Unindent(12.0f);
                if (i == selected) ImGui::SetItemDefaultFocus();
            }
            if (visibleCount == 0) ImGui::TextDisabled("No moon texture matches");
        }
        ImGui::EndChild();
        if (!ready || !regionOverride) ImGui::EndDisabled();
    }
    if (overrideChanged) editChanged = true;
    if (!celestialEnabled) ImGui::EndDisabled();
    if (detachedEdit && !providedEditData && editChanged) {
        CommitPresetEdit(editData, overrideMask, regionScoped);
    }
}

void MoveSectionInEditor(int from, int offset) {
    const int to = from + offset;
    if (from < 0 || to < 0 || from >= static_cast<int>(g_editorSections.size()) ||
        to >= static_cast<int>(g_editorSections.size())) {
        return;
    }
    std::swap(g_editorSections[from], g_editorSections[to]);
}

void MoveControlInEditor(int sectionIndex, int controlIndex, int offset) {
    if (sectionIndex < 0 || sectionIndex >= static_cast<int>(g_editorSections.size())) {
        return;
    }
    auto& controls = g_editorSections[sectionIndex].controlIds;
    const int to = controlIndex + offset;
    if (controlIndex < 0 || to < 0 || controlIndex >= static_cast<int>(controls.size()) ||
        to >= static_cast<int>(controls.size())) {
        return;
    }
    std::swap(controls[controlIndex], controls[to]);
}

void DrawEditorSliderLibrary() {
    if (g_selectedEditorSection < 0 ||
        g_selectedEditorSection >= static_cast<int>(g_editorSections.size())) {
        return;
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Add Control");
    ImGui::TextDisabled("Select a control to add it to this section.");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##favorite_search", "Search controls...", g_controlFilter, IM_ARRAYSIZE(g_controlFilter));
    if (ImGui::BeginChild("FavoriteSliderLibrary", ImVec2(0.0f, 154.0f), true)) {
        auto& controls = g_editorSections[g_selectedEditorSection].controlIds;
        const bool canAddControl = static_cast<int>(controls.size()) < kMaxFavoriteControlsPerSection;
        const char* categories[] = { "Weather", "General", "Atmosphere", "Celestial", "Experiment" };
        for (const char* category : categories) {
            bool categoryShown = false;
            for (const FavoriteExtraSpec& spec : kExtraSpecs) {
                if (strcmp(spec.category, category) != 0 ||
                    (g_controlFilter[0] && !TextContainsNoCase(spec.label, g_controlFilter) &&
                     !TextContainsNoCase(spec.category, g_controlFilter))) {
                    continue;
                }
                if (!categoryShown) {
                    ImGui::TextDisabled("%s", category);
                    categoryShown = true;
                }
                const char* existingSection = FindControlSectionName(g_editorSections, spec.id);
                const bool disabled = existingSection != nullptr || !canAddControl;
                if (disabled) ImGui::BeginDisabled();
                if (ImGui::Selectable(spec.label, false) && !disabled) {
                    controls.emplace_back(spec.id);
                    g_selectedEditorControl = static_cast<int>(controls.size()) - 1;
                }
                if (disabled) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (existingSection) {
                        ImGui::SetTooltip("Already in: %s", existingSection);
                    } else if (!canAddControl) {
                        ImGui::SetTooltip("This section already has the maximum of %d controls.", kMaxFavoriteControlsPerSection);
                    }
                }
            }
            for (const FavoriteSliderSpec& spec : kSliderSpecs) {
                if (strcmp(spec.category, category) != 0 ||
                    (g_controlFilter[0] && !TextContainsNoCase(spec.label, g_controlFilter) &&
                     !TextContainsNoCase(spec.category, g_controlFilter))) {
                    continue;
                }
                if (!categoryShown) {
                    ImGui::TextDisabled("%s", category);
                    categoryShown = true;
                }
                const char* existingSection = FindControlSectionName(g_editorSections, spec.id);
                const bool disabled = existingSection != nullptr || !canAddControl;
                if (disabled) ImGui::BeginDisabled();
                if (ImGui::Selectable(spec.label, false) && !disabled) {
                    controls.emplace_back(spec.id);
                    g_selectedEditorControl = static_cast<int>(controls.size()) - 1;
                }
                if (disabled) ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                    if (existingSection) {
                        ImGui::SetTooltip("Already in: %s", existingSection);
                    } else if (!canAddControl) {
                        ImGui::SetTooltip("This section already has the maximum of %d controls.", kMaxFavoriteControlsPerSection);
                    }
                }
            }
        }
    }
    ImGui::EndChild();
}

void DrawFavoritesEditor() {
    ImGui::SetNextWindowBgAlpha(0.96f);
    if (!ImGui::BeginPopupModal("Edit Favorites", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }
    ImGui::TextUnformatted("Build your Favorites tab");
    ImGui::TextDisabled("Select a section, then add and order the controls it contains.");
    ImGui::Separator();

    if (ImGui::BeginChild("FavoriteEditorSectionPane", ImVec2(225.0f, 435.0f), false)) {
        ImGui::TextDisabled("Sections");
        if (ImGui::BeginChild("FavoriteEditorSections", ImVec2(0.0f, 328.0f), true)) {
            for (int sectionIndex = 0; sectionIndex < static_cast<int>(g_editorSections.size()); ++sectionIndex) {
                const FavoriteSection& section = g_editorSections[sectionIndex];
                char label[96] = {};
                sprintf_s(label, "%s##favorite_section_%d", section.name[0] ? section.name : "New Section", sectionIndex);
                if (ImGui::Selectable(label, sectionIndex == g_selectedEditorSection)) {
                    g_selectedEditorSection = sectionIndex;
                    g_selectedEditorControl = -1;
                    g_controlFilter[0] = '\0';
                }
            }
            if (g_editorSections.empty()) {
                ImGui::TextDisabled("No sections yet.");
            }
        }
        ImGui::EndChild();
        const bool canAddSection = static_cast<int>(g_editorSections.size()) < kMaxFavoriteSections;
        if (!canAddSection) ImGui::BeginDisabled();
        if (ImGui::Button("+ Add Section", ImVec2(-1.0f, 0.0f))) {
            g_editorSections.emplace_back();
            g_selectedEditorSection = static_cast<int>(g_editorSections.size()) - 1;
            g_selectedEditorControl = -1;
            g_focusEditorSectionName = true;
        }
        if (!canAddSection) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !canAddSection) {
            ImGui::SetTooltip("Favorites supports up to %d sections.", kMaxFavoriteSections);
        }
        const bool canMoveSectionUp = g_selectedEditorSection > 0;
        if (!canMoveSectionUp) ImGui::BeginDisabled();
        if (ImGui::Button("Move Up##section", ImVec2(70.0f, 0.0f))) {
            MoveSectionInEditor(g_selectedEditorSection, -1);
            --g_selectedEditorSection;
        }
        if (!canMoveSectionUp) ImGui::EndDisabled();
        ImGui::SameLine();
        const bool canMoveSectionDown = g_selectedEditorSection >= 0 &&
            g_selectedEditorSection + 1 < static_cast<int>(g_editorSections.size());
        if (!canMoveSectionDown) ImGui::BeginDisabled();
        if (ImGui::Button("Move Down##section", ImVec2(78.0f, 0.0f))) {
            MoveSectionInEditor(g_selectedEditorSection, 1);
            ++g_selectedEditorSection;
        }
        if (!canMoveSectionDown) ImGui::EndDisabled();
        ImGui::SameLine();
        const bool hasSelectedSection = g_selectedEditorSection >= 0 &&
            g_selectedEditorSection < static_cast<int>(g_editorSections.size());
        if (!hasSelectedSection) ImGui::BeginDisabled();
        if (ImGui::Button("X##remove_section", ImVec2(32.0f, 0.0f))) {
            g_editorSections.erase(g_editorSections.begin() + g_selectedEditorSection);
            g_selectedEditorSection = min(g_selectedEditorSection, static_cast<int>(g_editorSections.size()) - 1);
            g_selectedEditorControl = -1;
        }
        if (!hasSelectedSection) ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Remove selected section");
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    const bool hasSelectedSection = g_selectedEditorSection >= 0 &&
        g_selectedEditorSection < static_cast<int>(g_editorSections.size());
    if (ImGui::BeginChild("FavoriteEditorSelectedSection", ImVec2(400.0f, 435.0f), true)) {
        if (hasSelectedSection) {
            FavoriteSection& section = g_editorSections[g_selectedEditorSection];
            ImGui::TextDisabled("Section Name");
            ImGui::SetNextItemWidth(-1.0f);
            if (g_focusEditorSectionName) {
                ImGui::SetKeyboardFocusHere();
                g_focusEditorSectionName = false;
            }
            ImGui::InputText("##favorite_section_name", section.name, IM_ARRAYSIZE(section.name));
            ImGui::Spacing();
            ImGui::TextDisabled("Controls");
            if (ImGui::BeginChild("FavoriteEditorControls", ImVec2(0.0f, 105.0f), true)) {
                for (int controlIndex = 0; controlIndex < static_cast<int>(section.controlIds.size()); ++controlIndex) {
                    if (!IsRegisteredControl(section.controlIds[controlIndex])) {
                        continue;
                    }
                    char label[128] = {};
                    sprintf_s(label, "%s  (%s)##favorite_control_%d",
                        ControlLabel(section.controlIds[controlIndex]),
                        ControlCategory(section.controlIds[controlIndex]),
                        controlIndex);
                    if (ImGui::Selectable(label, controlIndex == g_selectedEditorControl)) {
                        g_selectedEditorControl = controlIndex;
                    }
                }
                if (section.controlIds.empty()) {
                    ImGui::TextDisabled("Choose a control below to add it.");
                }
            }
            ImGui::EndChild();
            const bool hasSelectedControl = g_selectedEditorControl >= 0 &&
                g_selectedEditorControl < static_cast<int>(section.controlIds.size());
            const bool canMoveControlUp = hasSelectedControl && g_selectedEditorControl > 0;
            if (!canMoveControlUp) ImGui::BeginDisabled();
            if (ImGui::Button("Move Up##control", ImVec2(80.0f, 0.0f))) {
                MoveControlInEditor(g_selectedEditorSection, g_selectedEditorControl, -1);
                --g_selectedEditorControl;
            }
            if (!canMoveControlUp) ImGui::EndDisabled();
            ImGui::SameLine();
            const bool canMoveControlDown = hasSelectedControl &&
                g_selectedEditorControl + 1 < static_cast<int>(section.controlIds.size());
            if (!canMoveControlDown) ImGui::BeginDisabled();
            if (ImGui::Button("Move Down##control", ImVec2(90.0f, 0.0f))) {
                MoveControlInEditor(g_selectedEditorSection, g_selectedEditorControl, 1);
                ++g_selectedEditorControl;
            }
            if (!canMoveControlDown) ImGui::EndDisabled();
            ImGui::SameLine();
            if (!hasSelectedControl) ImGui::BeginDisabled();
            if (ImGui::Button("Remove##control", ImVec2(72.0f, 0.0f))) {
                section.controlIds.erase(section.controlIds.begin() + g_selectedEditorControl);
                g_selectedEditorControl = min(g_selectedEditorControl, static_cast<int>(section.controlIds.size()) - 1);
            }
            if (!hasSelectedControl) ImGui::EndDisabled();
            DrawEditorSliderLibrary();
        } else {
            ImGui::TextDisabled("Add a section to start building Favorites.");
        }
    }
    ImGui::EndChild();
    ImGui::Separator();
    const float footerWidth = 208.0f;
    const float footerX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - footerWidth;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), footerX));
    if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save", ImVec2(100.0f, 0.0f))) {
        for (FavoriteSection& section : g_editorSections) {
            if (!section.name[0]) {
                strcpy_s(section.name, "New Section");
            }
        }
        g_sections = g_editorSections;
        SaveFavoritesLayout();
        GUI_SetStatus("Favorites layout saved");
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace

void DrawSliderById(
    const char* id,
    WeatherPresetData* editData,
    WeatherPresetSourceMask* overrideMask,
    bool* editChanged) {
    const FavoriteSliderSpec* spec = FindSliderSpec(id);
    if (spec) {
        DrawFavoriteSlider(*spec, editData, overrideMask, editChanged);
    }
}

void DrawControlById(
    const char* id,
    WeatherPresetData* editData,
    WeatherPresetSourceMask* overrideMask,
    bool* editChanged) {
    if (FindSliderSpec(id)) {
        DrawSliderById(id, editData, overrideMask, editChanged);
        return;
    }
    const FavoriteExtraSpec* extra = FindExtraSpec(id);
    if (!extra) {
        return;
    }
    switch (extra->kind) {
    case FavoriteExtraKind::ForceClear:
    case FavoriteExtraKind::NoRain:
    case FavoriteExtraKind::NoDust:
    case FavoriteExtraKind::NoSnow:
    case FavoriteExtraKind::NoWind:
    case FavoriteExtraKind::NoFog:
        DrawFavoriteToggle(extra->kind, editData, overrideMask, editChanged);
        break;
    case FavoriteExtraKind::RayleighColor:
    case FavoriteExtraKind::VolumeFogColor:
    case FavoriteExtraKind::MieColor:
        DrawFavoriteColor(extra->kind, editData, overrideMask, editChanged);
        break;
    case FavoriteExtraKind::MilkywayTexture:
        DrawFavoriteMilkywayTexture(editData, overrideMask, editChanged);
        break;
    case FavoriteExtraKind::MoonTexture:
        DrawFavoriteMoonTexture(editData, overrideMask, editChanged);
        break;
    case FavoriteExtraKind::TimeControls:
        DrawTimeControls();
        break;
    }
}

void DrawFavoritesTab() {
    if (DrawDisabledTabBody()) {
        return;
    }
    LoadFavoritesLayout();
    const DWORD64 nowMs = GetTickCount64();
    if (g_lastFavoritesTickMs != 0 && nowMs - g_lastFavoritesTickMs > 250) {
        g_favoriteMoonTextureFilter[0] = '\0';
        g_favoriteMilkywayTextureFilter[0] = '\0';
    }
    g_lastFavoritesTickMs = nowMs;

    const bool emptyFavorites = g_sections.empty();
    const float addWidth = 30.0f;
    auto openFavoritesEditor = []() {
        g_editorSections = g_sections;
        g_controlFilter[0] = '\0';
        if (g_editorSections.empty()) {
            g_selectedEditorSection = -1;
        } else {
            g_selectedEditorSection = min(
                max(g_selectedEditorSection, 0),
                static_cast<int>(g_editorSections.size()) - 1);
        }
        g_selectedEditorControl = -1;
        g_focusEditorSectionName = false;
        g_openFavoritesEditorRequest = true;
    };
    auto drawAddButton = [&](const char* id) {
        if (ImGui::Button(id, ImVec2(addWidth, 0.0f))) {
            openFavoritesEditor();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Edit Favorites");
        }
    };

    if (emptyFavorites) {
        const float addX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - addWidth;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Press + to build your Favorites tab.");
        ImGui::SameLine();
        ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), addX));
        drawAddButton("+##add_favorites_section");
        if (g_openFavoritesEditorRequest) {
            ImGui::OpenPopup("Edit Favorites");
            g_openFavoritesEditorRequest = false;
        }
        DrawFavoritesEditor();
        return;
    }

    for (int sectionIndex = 0; sectionIndex < static_cast<int>(g_sections.size()); ++sectionIndex) {
        const FavoriteSection& section = g_sections[sectionIndex];
        ImGui::PushID(sectionIndex);
        ImGui::Spacing();
        if (sectionIndex == 0) {
            if (ImGui::BeginTable("FavoriteFirstSectionHeader", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("section", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("add", ImGuiTableColumnFlags_WidthFixed, addWidth);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::SeparatorText(section.name);
                ImGui::TableSetColumnIndex(1);
                drawAddButton("+##add_favorites_section_header");
                ImGui::EndTable();
            }
        } else {
            ImGui::SeparatorText(section.name);
        }
        for (int controlIndex = 0; controlIndex < static_cast<int>(section.controlIds.size()); ++controlIndex) {
            if (IsRegisteredControl(section.controlIds[controlIndex])) {
                ImGui::PushID(section.controlIds[controlIndex].c_str());
                DrawControlById(section.controlIds[controlIndex].c_str());
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }
    if (g_openFavoritesEditorRequest) {
        ImGui::OpenPopup("Edit Favorites");
        g_openFavoritesEditorRequest = false;
    }
    DrawFavoritesEditor();
}

} // namespace overlay_internal
