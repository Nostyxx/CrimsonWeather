#include "pch.h"
#include "runtime_shared.h"
#include "preset_model.h"
#include "preset_format.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace preset_internal {

constexpr int kPresetFormatVersion = 6;
bool TryParseBool(const std::string& text, bool& outValue) {
    std::string trimmed = TrimCopy(text);
    if (trimmed.empty()) return false;
    if (_stricmp(trimmed.c_str(), "1") == 0 || _stricmp(trimmed.c_str(), "true") == 0 ||
        _stricmp(trimmed.c_str(), "yes") == 0 || _stricmp(trimmed.c_str(), "on") == 0) {
        outValue = true;
        return true;
    }
    if (_stricmp(trimmed.c_str(), "0") == 0 || _stricmp(trimmed.c_str(), "false") == 0 ||
        _stricmp(trimmed.c_str(), "no") == 0 || _stricmp(trimmed.c_str(), "off") == 0) {
        outValue = false;
        return true;
    }
    return false;
}

bool TryParseFloat(const std::string& text, float& outValue) {
    std::string trimmed = TrimCopy(text);
    if (trimmed.empty()) return false;
    char* endPtr = nullptr;
    const float parsed = strtof(trimmed.c_str(), &endPtr);
    if (endPtr == trimmed.c_str() || (endPtr && *endPtr != '\0') || !std::isfinite(parsed)) {
        return false;
    }
    outValue = parsed;
    return true;
}

bool TryParseUInt32(const std::string& text, uint32_t& outValue) {
    std::string trimmed = TrimCopy(text);
    if (trimmed.empty()) return false;
    char* endPtr = nullptr;
    const unsigned long parsed = strtoul(trimmed.c_str(), &endPtr, 0);
    if (endPtr == trimmed.c_str() || (endPtr && *endPtr != '\0')) {
        return false;
    }
    outValue = static_cast<uint32_t>(parsed);
    return true;
}

struct PresetParseState {
    WeatherPresetData data{};
    WeatherPresetMask mask{};
    bool cloudAmountEnabledSeen = false;
    bool cloudHeightEnabledSeen = false;
    bool cloudDensityEnabledSeen = false;
    bool midCloudsEnabledSeen = false;
    bool highCloudsEnabledSeen = false;
    bool cloudAlphaEnabledSeen = false;
    bool cloudFadeRangeEnabledSeen = false;
    bool cloudDetailRatioEnabledSeen = false;
    bool cloudPhaseFrontEnabledSeen = false;
    bool cloudScatteringCoefficientEnabledSeen = false;
    bool cloudFlowEnabledSeen = false;
    bool cloudVisibleRangeEnabledSeen = false;
    bool snowAccumBoundaryAEnabledSeen = false;
    bool snowAccumBoundaryBEnabledSeen = false;
    bool snowCoverageThresholdEnabledSeen = false;
    bool rayleighHeightEnabledSeen = false;
    bool ozoneRatioEnabledSeen = false;
    bool rayleighScatteringColorEnabledSeen = false;
    bool exp2CEnabledSeen = false;
    bool exp2DEnabledSeen = false;
    bool cloudVariationEnabledSeen = false;
    bool nightSkyRotationEnabledSeen = false;
    bool nightSkyYawEnabledSeen = false;
    bool sunSizeEnabledSeen = false;
    bool sunLightIntensityEnabledSeen = false;
    bool sunYawEnabledSeen = false;
    bool sunPitchEnabledSeen = false;
    bool moonSizeEnabledSeen = false;
    bool moonLightIntensityEnabledSeen = false;
    bool moonYawEnabledSeen = false;
    bool moonPitchEnabledSeen = false;
    bool moonRollEnabledSeen = false;
    bool moonTextureEnabledSeen = false;
    bool milkywayTextureEnabledSeen = false;
    bool fogEnabledSeen = false;
    bool nativeFogEnabledSeen = false;
    bool volumeFogScatterColorEnabledSeen = false;
    bool mieScatterColorEnabledSeen = false;
    bool mieScaleHeightEnabledSeen = false;
    bool mieAerosolDensityEnabledSeen = false;
    bool mieAerosolAbsorptionEnabledSeen = false;
    bool heightFogBaselineEnabledSeen = false;
    bool heightFogFalloffEnabledSeen = false;
    bool puddleScaleEnabledSeen = false;
    bool renodxAuroraRegionMaskSeen = false;
    bool renodxAuroraGateEnabledSeen = false;
    bool sawLegacyAlias = false;
};

bool KeyEquals(const std::string& key, const char* expected) {
    return _stricmp(key.c_str(), expected) == 0;
}

void MarkPresetMaskForKey(const std::string& key, WeatherPresetMask& mask) {
    if (KeyEquals(key, "ForceClearSky")) mask.forceClearSky = true;
    else if (KeyEquals(key, "NoRain")) mask.noRain = true;
    else if (KeyEquals(key, "Rain")) mask.rain = true;
    else if (KeyEquals(key, "Thunder")) mask.thunder = true;
    else if (KeyEquals(key, "NoDust")) mask.noDust = true;
    else if (KeyEquals(key, "Dust")) mask.dust = true;
    else if (KeyEquals(key, "NoSnow")) mask.noSnow = true;
    else if (KeyEquals(key, "Snow")) mask.snow = true;
    else if (KeyEquals(key, "SnowAccumBoundaryAEnabled") || KeyEquals(key, "SnowAccumBoundaryA")) mask.snowAccumBoundaryA = true;
    else if (KeyEquals(key, "SnowAccumBoundaryBEnabled") || KeyEquals(key, "SnowAccumBoundaryB")) mask.snowAccumBoundaryB = true;
    else if (KeyEquals(key, "SnowCoverageThresholdEnabled") || KeyEquals(key, "SnowCoverageThreshold")) mask.snowCoverageThreshold = true;
    else if (KeyEquals(key, "VisualTimeOverride") || KeyEquals(key, "ProgressVisualTime") || KeyEquals(key, "ProgressVisualTimeMatchGameTime") || KeyEquals(key, "ProgressVisualTimeIntervalMs") || KeyEquals(key, "TimeHour")) mask.time = true;
    else if (KeyEquals(key, "CloudAmountEnabled") || KeyEquals(key, "CloudAmount")) mask.cloudAmount = true;
    else if (KeyEquals(key, "CloudHeightEnabled") || KeyEquals(key, "CloudHeight")) mask.cloudHeight = true;
    else if (KeyEquals(key, "CloudDensityEnabled") || KeyEquals(key, "CloudDensity")) mask.cloudDensity = true;
    else if (KeyEquals(key, "MidCloudsEnabled") || KeyEquals(key, "MidClouds") ||
             KeyEquals(key, "HighCloudsEnabled") || KeyEquals(key, "HighClouds") ||
             KeyEquals(key, "CloudScrollEnabled") || KeyEquals(key, "CloudScroll")) mask.midClouds = true;
    else if (KeyEquals(key, "HighCloudLayerEnabled") || KeyEquals(key, "HighCloudLayer")) mask.highClouds = true;
    else if (KeyEquals(key, "CloudAlphaEnabled") || KeyEquals(key, "CloudAlpha")) mask.cloudAlpha = true;
    else if (KeyEquals(key, "CloudFadeRangeEnabled") || KeyEquals(key, "CloudFadeRange")) mask.cloudFadeRange = true;
    else if (KeyEquals(key, "CloudDetailRatioEnabled") || KeyEquals(key, "CloudDetailRatio")) mask.cloudDetailRatio = true;
    else if (KeyEquals(key, "CloudPhaseFrontEnabled") || KeyEquals(key, "CloudPhaseFront")) mask.cloudPhaseFront = true;
    else if (KeyEquals(key, "CloudScatteringCoefficientEnabled") || KeyEquals(key, "CloudScatteringCoefficient")) mask.cloudScatteringCoefficient = true;
    else if (KeyEquals(key, "CloudFlowEnabled") || KeyEquals(key, "CloudFlow")) mask.cloudFlow = true;
    else if (KeyEquals(key, "CloudVisibleRangeEnabled") || KeyEquals(key, "CloudVisibleRange")) mask.cloudVisibleRange = true;
    else if (KeyEquals(key, "RayleighHeightEnabled") || KeyEquals(key, "RayleighHeight")) mask.rayleighHeight = true;
    else if (KeyEquals(key, "OzoneRatioEnabled") || KeyEquals(key, "OzoneRatio")) mask.ozoneRatio = true;
    else if (KeyEquals(key, "RayleighScatteringColorEnabled") ||
             KeyEquals(key, "RayleighScatteringColorR") ||
             KeyEquals(key, "RayleighScatteringColorG") ||
             KeyEquals(key, "RayleighScatteringColorB")) mask.rayleighScatteringColor = true;
    else if (KeyEquals(key, "2CEnabled") || KeyEquals(key, "2C")) mask.exp2C = true;
    else if (KeyEquals(key, "2DEnabled") || KeyEquals(key, "2D")) mask.exp2D = true;
    else if (KeyEquals(key, "CloudVariationEnabled") || KeyEquals(key, "CloudVariation") ||
             KeyEquals(key, "CloudThicknessEnabled") || KeyEquals(key, "CloudThickness")) mask.cloudVariation = true;
    else if (KeyEquals(key, "NightSkyTiltEnabled") || KeyEquals(key, "NightSkyTilt")) mask.nightSkyRotation = true;
    else if (KeyEquals(key, "NightSkyPhaseEnabled") || KeyEquals(key, "NightSkyPhase")) mask.nightSkyYaw = true;
    else if (KeyEquals(key, "SunSizeEnabled") || KeyEquals(key, "SunSize")) mask.sunSize = true;
    else if (KeyEquals(key, "SunLightIntensityEnabled") || KeyEquals(key, "SunLightIntensity")) mask.sunLightIntensity = true;
    else if (KeyEquals(key, "SunYawEnabled") || KeyEquals(key, "SunYaw")) mask.sunYaw = true;
    else if (KeyEquals(key, "SunPitchEnabled") || KeyEquals(key, "SunPitch")) mask.sunPitch = true;
    else if (KeyEquals(key, "MoonSizeEnabled") || KeyEquals(key, "MoonSize")) mask.moonSize = true;
    else if (KeyEquals(key, "MoonLightIntensityEnabled") || KeyEquals(key, "MoonLightIntensity")) mask.moonLightIntensity = true;
    else if (KeyEquals(key, "MoonYawEnabled") || KeyEquals(key, "MoonYaw")) mask.moonYaw = true;
    else if (KeyEquals(key, "MoonPitchEnabled") || KeyEquals(key, "MoonPitch")) mask.moonPitch = true;
    else if (KeyEquals(key, "MoonRollEnabled") || KeyEquals(key, "MoonRoll")) mask.moonRoll = true;
    else if (KeyEquals(key, "MoonTextureEnabled") || KeyEquals(key, "MoonTexture")) mask.moonTexture = true;
    else if (KeyEquals(key, "MilkywayTextureEnabled") || KeyEquals(key, "MilkywayTexture")) mask.milkywayTexture = true;
    else if (KeyEquals(key, "FogEnabled") || KeyEquals(key, "Fog")) mask.fog = true;
    else if (KeyEquals(key, "NativeFogEnabled") || KeyEquals(key, "NativeFog") ||
             KeyEquals(key, "PlainFogEnabled") || KeyEquals(key, "PlainFog")) mask.nativeFog = true;
    else if (KeyEquals(key, "VolumeFogScatterColorEnabled") ||
             KeyEquals(key, "VolumeFogScatterColorR") ||
             KeyEquals(key, "VolumeFogScatterColorG") ||
             KeyEquals(key, "VolumeFogScatterColorB") ||
             KeyEquals(key, "VolumeFogScatterColorA")) mask.volumeFogScatterColor = true;
    else if (KeyEquals(key, "MieScatterColorEnabled") ||
             KeyEquals(key, "MieScatterColorR") ||
             KeyEquals(key, "MieScatterColorG") ||
             KeyEquals(key, "MieScatterColorB") ||
             KeyEquals(key, "MieScatterColorA")) mask.mieScatterColor = true;
    else if (KeyEquals(key, "MieScaleHeightEnabled") || KeyEquals(key, "MieScaleHeight")) mask.mieScaleHeight = true;
    else if (KeyEquals(key, "MieAerosolDensityEnabled") || KeyEquals(key, "MieAerosolDensity")) mask.mieAerosolDensity = true;
    else if (KeyEquals(key, "MieAerosolAbsorptionEnabled") || KeyEquals(key, "MieAerosolAbsorption")) mask.mieAerosolAbsorption = true;
    else if (KeyEquals(key, "HeightFogBaselineEnabled") || KeyEquals(key, "HeightFogBaseline")) mask.heightFogBaseline = true;
    else if (KeyEquals(key, "HeightFogFalloffEnabled") || KeyEquals(key, "HeightFogFalloff")) mask.heightFogFalloff = true;
    else if (KeyEquals(key, "NoFog")) mask.noFog = true;
    else if (KeyEquals(key, "Wind")) mask.wind = true;
    else if (KeyEquals(key, "NoWind")) mask.noWind = true;
    else if (KeyEquals(key, "PuddleScaleEnabled") || KeyEquals(key, "PuddleScale")) mask.puddleScale = true;
}

bool ParseSnowPresetKeyValue(const std::string& key, const std::string& value, PresetParseState& state) {
    bool boolValue = false;
    float floatValue = 0.0f;
    WeatherPresetData& data = state.data;

    if (KeyEquals(key, "SnowAccumBoundaryAEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.snowAccumBoundaryAEnabled = boolValue;
            state.snowAccumBoundaryAEnabledSeen = true;
        }
        return true;
    }
    if (KeyEquals(key, "SnowAccumBoundaryA")) {
        if (TryParseFloat(value, floatValue)) data.snowAccumBoundaryA = floatValue;
        return true;
    }
    if (KeyEquals(key, "SnowAccumBoundaryBEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.snowAccumBoundaryBEnabled = boolValue;
            state.snowAccumBoundaryBEnabledSeen = true;
        }
        return true;
    }
    if (KeyEquals(key, "SnowAccumBoundaryB")) {
        if (TryParseFloat(value, floatValue)) data.snowAccumBoundaryB = floatValue;
        return true;
    }
    if (KeyEquals(key, "SnowCoverageThresholdEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.snowCoverageThresholdEnabled = boolValue;
            state.snowCoverageThresholdEnabledSeen = true;
        }
        return true;
    }
    if (KeyEquals(key, "SnowCoverageThreshold")) {
        if (TryParseFloat(value, floatValue)) data.snowCoverageThreshold = floatValue;
        return true;
    }
    return false;
}

void ParsePresetKeyValue(const std::string& key, const std::string& value, PresetParseState& state) {
    bool boolValue = false;
    float floatValue = 0.0f;
    uint32_t uintValue = 0;
    WeatherPresetData& data = state.data;
    MarkPresetMaskForKey(key, state.mask);

    if (KeyEquals(key, "ForceClearSky")) {
        if (TryParseBool(value, boolValue)) data.forceClearSky = boolValue;
    } else if (KeyEquals(key, "NoRain")) {
        if (TryParseBool(value, boolValue)) data.noRain = boolValue;
    } else if (KeyEquals(key, "Rain")) {
        if (TryParseFloat(value, floatValue)) data.rain = floatValue;
    } else if (KeyEquals(key, "Thunder")) {
        if (TryParseFloat(value, floatValue)) data.thunder = floatValue;
    } else if (KeyEquals(key, "NoDust")) {
        if (TryParseBool(value, boolValue)) data.noDust = boolValue;
    } else if (KeyEquals(key, "Dust")) {
        if (TryParseFloat(value, floatValue)) data.dust = floatValue;
    } else if (KeyEquals(key, "NoSnow")) {
        if (TryParseBool(value, boolValue)) data.noSnow = boolValue;
    } else if (KeyEquals(key, "Snow")) {
        if (TryParseFloat(value, floatValue)) data.snow = floatValue;
    } else if (ParseSnowPresetKeyValue(key, value, state)) {
    } else if (KeyEquals(key, "VisualTimeOverride")) {
        if (TryParseBool(value, boolValue)) data.visualTimeOverride = boolValue;
    } else if (KeyEquals(key, "ProgressVisualTime")) {
        if (TryParseBool(value, boolValue)) data.progressVisualTime = boolValue;
    } else if (KeyEquals(key, "ProgressVisualTimeMatchGameTime")) {
        if (TryParseBool(value, boolValue)) data.progressVisualTimeMatchGameTime = boolValue;
    } else if (KeyEquals(key, "ProgressVisualTimeIntervalMs")) {
        if (TryParseFloat(value, floatValue)) data.progressVisualTimeIntervalMs = floatValue;
    } else if (KeyEquals(key, "TimeHour")) {
        if (TryParseFloat(value, floatValue)) data.timeHour = floatValue;
    } else if (KeyEquals(key, "CloudAmountEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.cloudAmountEnabled = boolValue;
            state.cloudAmountEnabledSeen = true;
        }
    } else if (KeyEquals(key, "CloudAmount")) {
        if (TryParseFloat(value, floatValue)) data.cloudAmount = floatValue;
    } else if (KeyEquals(key, "CloudHeightEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.cloudHeightEnabled = boolValue;
            state.cloudHeightEnabledSeen = true;
        }
    } else if (KeyEquals(key, "CloudHeight")) {
        if (TryParseFloat(value, floatValue)) data.cloudHeight = floatValue;
    } else if (KeyEquals(key, "CloudDensityEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.cloudDensityEnabled = boolValue;
            state.cloudDensityEnabledSeen = true;
        }
    } else if (KeyEquals(key, "CloudDensity")) {
        if (TryParseFloat(value, floatValue)) data.cloudDensity = floatValue;
    } else if (KeyEquals(key, "MidCloudsEnabled") ||
               KeyEquals(key, "HighCloudsEnabled") ||
               KeyEquals(key, "CloudScrollEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.midCloudsEnabled = boolValue;
            state.midCloudsEnabledSeen = true;
            if (!KeyEquals(key, "MidCloudsEnabled")) state.sawLegacyAlias = true;
        }
    } else if (KeyEquals(key, "MidClouds") ||
               KeyEquals(key, "HighClouds") ||
               KeyEquals(key, "CloudScroll")) {
        if (TryParseFloat(value, floatValue)) {
            data.midClouds = floatValue;
            if (!KeyEquals(key, "MidClouds")) state.sawLegacyAlias = true;
        }
    } else if (KeyEquals(key, "HighCloudLayerEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.highCloudsEnabled = boolValue;
            state.highCloudsEnabledSeen = true;
        }
    } else if (KeyEquals(key, "HighCloudLayer")) {
        if (TryParseFloat(value, floatValue)) data.highClouds = floatValue;
    } else if (KeyEquals(key, "CloudAlphaEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.cloudAlphaEnabled = boolValue;
            state.cloudAlphaEnabledSeen = true;
        }
    } else if (KeyEquals(key, "CloudAlpha")) {
        if (TryParseFloat(value, floatValue)) data.cloudAlpha = floatValue;
    } else if (KeyEquals(key, "CloudFadeRangeEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.cloudFadeRangeEnabled = boolValue;
            state.cloudFadeRangeEnabledSeen = true;
        }
    } else if (KeyEquals(key, "CloudFadeRange")) {
        if (TryParseFloat(value, floatValue)) data.cloudFadeRange = floatValue;
    } else if (KeyEquals(key, "CloudDetailRatioEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.cloudDetailRatioEnabled = boolValue;
            state.cloudDetailRatioEnabledSeen = true;
        }
    } else if (KeyEquals(key, "CloudDetailRatio")) {
        if (TryParseFloat(value, floatValue)) data.cloudDetailRatio = floatValue;
    } else if (KeyEquals(key, "CloudPhaseFrontEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.cloudPhaseFrontEnabled = boolValue;
            state.cloudPhaseFrontEnabledSeen = true;
        }
    } else if (KeyEquals(key, "CloudPhaseFront")) {
        if (TryParseFloat(value, floatValue)) data.cloudPhaseFront = floatValue;
    } else if (KeyEquals(key, "CloudScatteringCoefficientEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.cloudScatteringCoefficientEnabled = boolValue;
            state.cloudScatteringCoefficientEnabledSeen = true;
        }
    } else if (KeyEquals(key, "CloudScatteringCoefficient")) {
        if (TryParseFloat(value, floatValue)) data.cloudScatteringCoefficient = floatValue;
    } else if (KeyEquals(key, "CloudFlowEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.cloudFlowEnabled = boolValue;
            state.cloudFlowEnabledSeen = true;
        }
    } else if (KeyEquals(key, "CloudFlow")) {
        if (TryParseFloat(value, floatValue)) data.cloudFlow = floatValue;
    } else if (KeyEquals(key, "CloudVisibleRangeEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.cloudVisibleRangeEnabled = boolValue;
            state.cloudVisibleRangeEnabledSeen = true;
        }
    } else if (KeyEquals(key, "CloudVisibleRange")) {
        if (TryParseFloat(value, floatValue)) data.cloudVisibleRange = floatValue;
    } else if (KeyEquals(key, "RayleighHeightEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.rayleighHeightEnabled = boolValue;
            state.rayleighHeightEnabledSeen = true;
        }
    } else if (KeyEquals(key, "RayleighHeight")) {
        if (TryParseFloat(value, floatValue)) data.rayleighHeight = floatValue;
    } else if (KeyEquals(key, "OzoneRatioEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.ozoneRatioEnabled = boolValue;
            state.ozoneRatioEnabledSeen = true;
        }
    } else if (KeyEquals(key, "OzoneRatio")) {
        if (TryParseFloat(value, floatValue)) data.ozoneRatio = floatValue;
    } else if (KeyEquals(key, "RayleighScatteringColorEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.rayleighScatteringColorEnabled = boolValue;
            state.rayleighScatteringColorEnabledSeen = true;
        }
    } else if (KeyEquals(key, "RayleighScatteringColorR")) {
        if (TryParseFloat(value, floatValue)) data.rayleighScatteringColor.r = floatValue;
    } else if (KeyEquals(key, "RayleighScatteringColorG")) {
        if (TryParseFloat(value, floatValue)) data.rayleighScatteringColor.g = floatValue;
    } else if (KeyEquals(key, "RayleighScatteringColorB")) {
        if (TryParseFloat(value, floatValue)) data.rayleighScatteringColor.b = floatValue;
    } else if (KeyEquals(key, "2CEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.exp2CEnabled = boolValue;
            state.exp2CEnabledSeen = true;
        }
    } else if (KeyEquals(key, "2C")) {
        if (TryParseFloat(value, floatValue)) data.exp2C = floatValue;
    } else if (KeyEquals(key, "2DEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.exp2DEnabled = boolValue;
            state.exp2DEnabledSeen = true;
        }
    } else if (KeyEquals(key, "2D")) {
        if (TryParseFloat(value, floatValue)) data.exp2D = floatValue;
    } else if (KeyEquals(key, "CloudVariationEnabled") ||
               KeyEquals(key, "CloudThicknessEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.cloudVariationEnabled = boolValue;
            state.cloudVariationEnabledSeen = true;
        }
    } else if (KeyEquals(key, "CloudVariation") ||
               KeyEquals(key, "CloudThickness")) {
        if (TryParseFloat(value, floatValue)) data.cloudVariation = floatValue;
    } else if (KeyEquals(key, "NightSkyTiltEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.nightSkyRotationEnabled = boolValue;
            state.nightSkyRotationEnabledSeen = true;
        }
    } else if (KeyEquals(key, "NightSkyTilt")) {
        if (TryParseFloat(value, floatValue)) data.nightSkyRotation = floatValue;
    } else if (KeyEquals(key, "NightSkyPhaseEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.nightSkyYawEnabled = boolValue;
            state.nightSkyYawEnabledSeen = true;
        }
    } else if (KeyEquals(key, "NightSkyPhase")) {
        if (TryParseFloat(value, floatValue)) data.nightSkyYaw = floatValue;
    } else if (KeyEquals(key, "SunSizeEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.sunSizeEnabled = boolValue;
            state.sunSizeEnabledSeen = true;
        }
    } else if (KeyEquals(key, "SunSize")) {
        if (TryParseFloat(value, floatValue)) data.sunSize = floatValue;
    } else if (KeyEquals(key, "SunLightIntensityEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.sunLightIntensityEnabled = boolValue;
            state.sunLightIntensityEnabledSeen = true;
        }
    } else if (KeyEquals(key, "SunLightIntensity")) {
        if (TryParseFloat(value, floatValue)) data.sunLightIntensity = floatValue;
    } else if (KeyEquals(key, "SunYawEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.sunYawEnabled = boolValue;
            state.sunYawEnabledSeen = true;
        }
    } else if (KeyEquals(key, "SunYaw")) {
        if (TryParseFloat(value, floatValue)) data.sunYaw = floatValue;
    } else if (KeyEquals(key, "SunPitchEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.sunPitchEnabled = boolValue;
            state.sunPitchEnabledSeen = true;
        }
    } else if (KeyEquals(key, "SunPitch")) {
        if (TryParseFloat(value, floatValue)) data.sunPitch = floatValue;
    } else if (KeyEquals(key, "MoonSizeEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.moonSizeEnabled = boolValue;
            state.moonSizeEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MoonSize")) {
        if (TryParseFloat(value, floatValue)) data.moonSize = floatValue;
    } else if (KeyEquals(key, "MoonLightIntensityEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.moonLightIntensityEnabled = boolValue;
            state.moonLightIntensityEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MoonLightIntensity")) {
        if (TryParseFloat(value, floatValue)) data.moonLightIntensity = floatValue;
    } else if (KeyEquals(key, "MoonYawEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.moonYawEnabled = boolValue;
            state.moonYawEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MoonYaw")) {
        if (TryParseFloat(value, floatValue)) data.moonYaw = floatValue;
    } else if (KeyEquals(key, "MoonPitchEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.moonPitchEnabled = boolValue;
            state.moonPitchEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MoonPitch")) {
        if (TryParseFloat(value, floatValue)) data.moonPitch = floatValue;
    } else if (KeyEquals(key, "MoonRollEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.moonRollEnabled = boolValue;
            state.moonRollEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MoonRoll")) {
        if (TryParseFloat(value, floatValue)) data.moonRoll = floatValue;
    } else if (KeyEquals(key, "MoonTextureEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.moonTextureEnabled = boolValue;
            state.moonTextureEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MoonTexture")) {
        data.moonTexture = TrimCopy(value);
    } else if (KeyEquals(key, "MilkywayTextureEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.milkywayTextureEnabled = boolValue;
            state.milkywayTextureEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MilkywayTexture")) {
        data.milkywayTexture = TrimCopy(value);
    } else if (KeyEquals(key, "FogEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.fogEnabled = boolValue;
            state.fogEnabledSeen = true;
        }
    } else if (KeyEquals(key, "Fog")) {
        if (TryParseFloat(value, floatValue)) data.fogPercent = floatValue;
    } else if (KeyEquals(key, "NativeFogEnabled") || KeyEquals(key, "PlainFogEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.nativeFogEnabled = boolValue;
            state.nativeFogEnabledSeen = true;
        }
    } else if (KeyEquals(key, "NativeFog") || KeyEquals(key, "PlainFog")) {
        if (TryParseFloat(value, floatValue)) data.nativeFog = floatValue;
    } else if (KeyEquals(key, "VolumeFogScatterColorEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.volumeFogScatterColorEnabled = boolValue;
            state.volumeFogScatterColorEnabledSeen = true;
        }
    } else if (KeyEquals(key, "VolumeFogScatterColorR")) {
        if (TryParseFloat(value, floatValue)) data.volumeFogScatterColor.r = floatValue;
    } else if (KeyEquals(key, "VolumeFogScatterColorG")) {
        if (TryParseFloat(value, floatValue)) data.volumeFogScatterColor.g = floatValue;
    } else if (KeyEquals(key, "VolumeFogScatterColorB")) {
        if (TryParseFloat(value, floatValue)) data.volumeFogScatterColor.b = floatValue;
    } else if (KeyEquals(key, "VolumeFogScatterColorA")) {
        if (TryParseFloat(value, floatValue)) data.volumeFogScatterColor.a = floatValue;
    } else if (KeyEquals(key, "MieScatterColorEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.mieScatterColorEnabled = boolValue;
            state.mieScatterColorEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MieScatterColorR")) {
        if (TryParseFloat(value, floatValue)) data.mieScatterColor.r = floatValue;
    } else if (KeyEquals(key, "MieScatterColorG")) {
        if (TryParseFloat(value, floatValue)) data.mieScatterColor.g = floatValue;
    } else if (KeyEquals(key, "MieScatterColorB")) {
        if (TryParseFloat(value, floatValue)) data.mieScatterColor.b = floatValue;
    } else if (KeyEquals(key, "MieScatterColorA")) {
        if (TryParseFloat(value, floatValue)) data.mieScatterColor.a = floatValue;
    } else if (KeyEquals(key, "MieScaleHeightEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.mieScaleHeightEnabled = boolValue;
            state.mieScaleHeightEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MieScaleHeight")) {
        if (TryParseFloat(value, floatValue)) data.mieScaleHeight = floatValue;
    } else if (KeyEquals(key, "MieAerosolDensityEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.mieAerosolDensityEnabled = boolValue;
            state.mieAerosolDensityEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MieAerosolDensity")) {
        if (TryParseFloat(value, floatValue)) data.mieAerosolDensity = floatValue;
    } else if (KeyEquals(key, "MieAerosolAbsorptionEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.mieAerosolAbsorptionEnabled = boolValue;
            state.mieAerosolAbsorptionEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MieAerosolAbsorption")) {
        if (TryParseFloat(value, floatValue)) data.mieAerosolAbsorption = floatValue;
    } else if (KeyEquals(key, "HeightFogBaselineEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.heightFogBaselineEnabled = boolValue;
            state.heightFogBaselineEnabledSeen = true;
        }
    } else if (KeyEquals(key, "HeightFogBaseline")) {
        if (TryParseFloat(value, floatValue)) data.heightFogBaseline = floatValue;
    } else if (KeyEquals(key, "HeightFogFalloffEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.heightFogFalloffEnabled = boolValue;
            state.heightFogFalloffEnabledSeen = true;
        }
    } else if (KeyEquals(key, "HeightFogFalloff")) {
        if (TryParseFloat(value, floatValue)) data.heightFogFalloff = floatValue;
    } else if (KeyEquals(key, "NoFog")) {
        if (TryParseBool(value, boolValue)) data.noFog = boolValue;
    } else if (KeyEquals(key, "Wind")) {
        if (TryParseFloat(value, floatValue)) data.wind = floatValue;
    } else if (KeyEquals(key, "NoWind")) {
        if (TryParseBool(value, boolValue)) data.noWind = boolValue;
    } else if (KeyEquals(key, "PuddleScaleEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.puddleScaleEnabled = boolValue;
            state.puddleScaleEnabledSeen = true;
        }
    } else if (KeyEquals(key, "PuddleScale")) {
        if (TryParseFloat(value, floatValue)) data.puddleScale = floatValue;
    } else if (KeyEquals(key, "AuroraEnabled") || KeyEquals(key, "AuroraGateEnabled") || KeyEquals(key, "RenoDxAuroraEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.renodxAuroraRegionMaskEnabled = true;
            data.renodxAuroraGateEnabled = boolValue;
            state.renodxAuroraRegionMaskSeen = true;
            state.renodxAuroraGateEnabledSeen = true;
        }
    } else if (KeyEquals(key, "AuroraRegionMask") || KeyEquals(key, "RenoDxAuroraRegionMask") || KeyEquals(key, "RenoDXAuroraRegionMask")) {
        if (TryParseUInt32(value, uintValue)) {
            data.renodxAuroraRegionMaskEnabled = true;
            data.renodxAuroraRegionMask = uintValue & 126u;
            state.renodxAuroraRegionMaskSeen = true;
        }
    }
}

void NormalizeLoadedPreset(PresetParseState& state, const char* path, bool extendedSliderRange) {
    WeatherPresetData& data = state.data;

    if (!state.cloudAmountEnabledSeen) data.cloudAmountEnabled = !FloatNearlyEqual(data.cloudAmount, 1.0f);
    if (!state.cloudHeightEnabledSeen) data.cloudHeightEnabled = !FloatNearlyEqual(data.cloudHeight, 1.0f);
    if (!state.cloudDensityEnabledSeen) data.cloudDensityEnabled = !FloatNearlyEqual(data.cloudDensity, 1.0f);
    if (!state.midCloudsEnabledSeen) data.midCloudsEnabled = !FloatNearlyEqual(data.midClouds, 1.0f);
    if (!state.highCloudsEnabledSeen) data.highCloudsEnabled = !FloatNearlyEqual(data.highClouds, 1.0f);
    if (!state.cloudAlphaEnabledSeen) data.cloudAlphaEnabled = false;
    if (!state.cloudFadeRangeEnabledSeen) data.cloudFadeRangeEnabled = false;
    if (!state.cloudDetailRatioEnabledSeen) data.cloudDetailRatioEnabled = false;
    if (!state.cloudPhaseFrontEnabledSeen) data.cloudPhaseFrontEnabled = false;
    if (!state.cloudScatteringCoefficientEnabledSeen) data.cloudScatteringCoefficientEnabled = false;
    if (!state.cloudFlowEnabledSeen) data.cloudFlowEnabled = false;
    if (!state.cloudVisibleRangeEnabledSeen) data.cloudVisibleRangeEnabled = false;
    if (!state.rayleighHeightEnabledSeen) data.rayleighHeightEnabled = false;
    if (!state.ozoneRatioEnabledSeen) data.ozoneRatioEnabled = false;
    if (!state.rayleighScatteringColorEnabledSeen) data.rayleighScatteringColorEnabled = false;
    if (!state.exp2CEnabledSeen) data.exp2CEnabled = false;
    if (!state.exp2DEnabledSeen) data.exp2DEnabled = false;
    if (!state.cloudVariationEnabledSeen) data.cloudVariationEnabled = false;
    if (!state.nightSkyRotationEnabledSeen) data.nightSkyRotationEnabled = false;
    if (!state.nightSkyYawEnabledSeen) data.nightSkyYawEnabled = false;
    if (!state.sunSizeEnabledSeen) data.sunSizeEnabled = false;
    if (!state.sunLightIntensityEnabledSeen) data.sunLightIntensityEnabled = false;
    if (!state.sunYawEnabledSeen) data.sunYawEnabled = false;
    if (!state.sunPitchEnabledSeen) data.sunPitchEnabled = false;
    if (!state.moonSizeEnabledSeen) data.moonSizeEnabled = false;
    if (!state.moonLightIntensityEnabledSeen) data.moonLightIntensityEnabled = false;
    if (!state.moonYawEnabledSeen) data.moonYawEnabled = false;
    if (!state.moonPitchEnabledSeen) data.moonPitchEnabled = false;
    if (!state.moonRollEnabledSeen) data.moonRollEnabled = false;
    if (!state.moonTextureEnabledSeen) data.moonTextureEnabled = !data.moonTexture.empty();
    if (EqualsNoCase(data.moonTexture, "Native")) {
        data.moonTexture.clear();
        data.moonTextureEnabled = false;
    }
    if (!data.moonTextureEnabled) {
        data.moonTexture.clear();
    }
    if (!state.milkywayTextureEnabledSeen) data.milkywayTextureEnabled = !data.milkywayTexture.empty();
    if (EqualsNoCase(data.milkywayTexture, "Native")) {
        data.milkywayTexture.clear();
        data.milkywayTextureEnabled = false;
    }
    if (!data.milkywayTextureEnabled) {
        data.milkywayTexture.clear();
    }
    if (!state.fogEnabledSeen) data.fogEnabled = !FloatNearlyEqual(data.fogPercent, 0.0f);
    if (!state.volumeFogScatterColorEnabledSeen) data.volumeFogScatterColorEnabled = false;
    if (!state.mieScatterColorEnabledSeen) data.mieScatterColorEnabled = false;
    if (!state.mieScaleHeightEnabledSeen) data.mieScaleHeightEnabled = false;
    if (!state.mieAerosolDensityEnabledSeen) data.mieAerosolDensityEnabled = false;
    if (!state.mieAerosolAbsorptionEnabledSeen) data.mieAerosolAbsorptionEnabled = false;
    if (!state.heightFogBaselineEnabledSeen) data.heightFogBaselineEnabled = false;
    if (!state.heightFogFalloffEnabledSeen) data.heightFogFalloffEnabled = false;
    if (!state.puddleScaleEnabledSeen) data.puddleScaleEnabled = !FloatNearlyEqual(data.puddleScale, 0.0f);

    data.rain = ClampPresetRain(extendedSliderRange, data.rain);
    data.thunder = ClampPresetThunder(extendedSliderRange, data.thunder);
    data.dust = ClampPresetDust(extendedSliderRange, data.dust);
    data.snow = ClampPresetSnow(extendedSliderRange, data.snow);
    if (!state.snowAccumBoundaryAEnabledSeen) data.snowAccumBoundaryAEnabled = false;
    if (!state.snowAccumBoundaryBEnabledSeen) data.snowAccumBoundaryBEnabled = false;
    if (!state.snowCoverageThresholdEnabledSeen) data.snowCoverageThresholdEnabled = false;
    data.snowAccumBoundaryA = ClampPresetSnowBoundary(data.snowAccumBoundaryA);
    data.snowAccumBoundaryB = ClampPresetSnowBoundary(data.snowAccumBoundaryB);
    data.snowCoverageThreshold = ClampPresetSnowBoundary(data.snowCoverageThreshold);
    data.progressVisualTime = data.visualTimeOverride && data.progressVisualTime;
    data.progressVisualTimeMatchGameTime = data.progressVisualTime && data.progressVisualTimeMatchGameTime;
    data.progressVisualTimeIntervalMs = ClampPresetFloat(data.progressVisualTimeIntervalMs, 0.0f, 5000.0f);
    data.timeHour = NormalizeHour24(data.timeHour);
    data.nativeFog = ClampPresetNativeFog(extendedSliderRange, data.nativeFog);
    if (!state.nativeFogEnabledSeen) data.nativeFogEnabled = !FloatNearlyEqual(data.nativeFog, 1.0f);
    data.cloudAlpha = ClampPresetCloudAlpha(extendedSliderRange, data.cloudAlpha);
    data.cloudFadeRange = ClampPresetCloudFadeRange(extendedSliderRange, data.cloudFadeRange);
    data.cloudDetailRatio = ClampPresetCloudDetailRatio(data.cloudDetailRatio);
    data.cloudPhaseFront = ClampPresetCloudPhaseFront(extendedSliderRange, data.cloudPhaseFront);
    data.cloudScatteringCoefficient = ClampPresetCloudScatteringCoefficient(extendedSliderRange, data.cloudScatteringCoefficient);
    data.cloudFlow = ClampPresetCloudFlow(extendedSliderRange, data.cloudFlow);
    data.cloudVisibleRange = ClampPresetCloudVisibleRange(data.cloudVisibleRange);
    data.rayleighHeight = ClampPresetRayleighHeight(extendedSliderRange, data.rayleighHeight);
    data.ozoneRatio = ClampPresetOzoneRatio(extendedSliderRange, data.ozoneRatio);
    data.rayleighScatteringColor = ClampPresetColor(data.rayleighScatteringColor, false);
    data.sunLightIntensity = ClampPresetLightIntensity(extendedSliderRange, data.sunLightIntensity);
    data.moonLightIntensity = ClampPresetLightIntensity(extendedSliderRange, data.moonLightIntensity);
    data.volumeFogScatterColor = ClampPresetColor(data.volumeFogScatterColor, true);
    data.mieScatterColor = ClampPresetColor(data.mieScatterColor, true);
    data.mieScaleHeight = ClampPresetMieScaleHeight(extendedSliderRange, data.mieScaleHeight);
    data.mieAerosolDensity = ClampPresetMieDensity(extendedSliderRange, data.mieAerosolDensity);
    data.mieAerosolAbsorption = ClampPresetMieAbsorption(extendedSliderRange, data.mieAerosolAbsorption);
    data.heightFogBaseline = ClampPresetHeightFogBaseline(extendedSliderRange, data.heightFogBaseline);
    data.heightFogFalloff = ClampPresetHeightFogFalloff(extendedSliderRange, data.heightFogFalloff);
    data.exp2C = ClampPresetCloudWide(extendedSliderRange, data.exp2C);
    data.exp2D = ClampPresetCloudWide(extendedSliderRange, data.exp2D);
    data.cloudVariation = ClampPresetCloudWide(extendedSliderRange, data.cloudVariation);
    data.nightSkyRotation = ClampPresetPitch(extendedSliderRange, data.nightSkyRotation);
    data.nightSkyYaw = ClampPresetYaw(extendedSliderRange, data.nightSkyYaw);
    data.sunSize = ClampPresetSunSize(extendedSliderRange, data.sunSize);
    data.sunYaw = ClampPresetYaw(extendedSliderRange, data.sunYaw);
    data.sunPitch = ClampPresetPitch(extendedSliderRange, data.sunPitch);
    data.moonSize = ClampPresetMoonSize(extendedSliderRange, data.moonSize);
    data.moonYaw = ClampPresetYaw(extendedSliderRange, data.moonYaw);
    data.moonPitch = ClampPresetPitch(extendedSliderRange, data.moonPitch);
    data.moonRoll = ClampPresetYaw(extendedSliderRange, data.moonRoll);
    data.fogPercent = ClampPresetFogPercent(extendedSliderRange, data.fogPercent);
    data.wind = ClampPresetWind(extendedSliderRange, data.wind);
    data.puddleScale = ClampPresetPuddleScale(extendedSliderRange, data.puddleScale);
    data.cloudAmount = ClampPresetCloudAmount(extendedSliderRange, data.cloudAmount);
    data.cloudHeight = ClampPresetCloudHeight(extendedSliderRange, data.cloudHeight);
    data.cloudDensity = ClampPresetCloudDensity(extendedSliderRange, data.cloudDensity);
    data.midClouds = ClampPresetCloudWide(extendedSliderRange, data.midClouds);
    data.highClouds = ClampPresetCloudWide(extendedSliderRange, data.highClouds);
    data.renodxAuroraRegionMaskEnabled = state.renodxAuroraRegionMaskSeen;
    if (data.renodxAuroraRegionMaskEnabled && !state.renodxAuroraGateEnabledSeen) {
        data.renodxAuroraGateEnabled = true;
    }
    data.renodxAuroraRegionMask &= 126u;

    if (state.sawLegacyAlias) {
        Log("[preset] loaded legacy cloud aliases from %s\n", path);
    }
}

std::string FormatPresetBool(bool value) {
    return value ? "1" : "0";
}

std::string FormatPresetFloat(float value) {
    char buf[64] = {};
    sprintf_s(buf, "%.4f", value);
    return buf;
}

std::string FormatPresetString(std::string value) {
    value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
    value.erase(std::remove(value.begin(), value.end(), '\n'), value.end());
    return value;
}

std::string FormatCommunityMetadataString(const char* value, size_t maxLen = 160) {
    std::string out = FormatPresetString(value ? value : "");
    out.erase(std::remove_if(out.begin(), out.end(), [](unsigned char c) {
        return c < 0x20 || c == 0x7F;
    }), out.end());
    if (out.size() > maxLen) {
        out.resize(maxLen);
    }
    return out;
}

void AppendPresetLine(std::string& out, const char* text) {
    out += text;
    out += '\n';
}

void AppendPresetKeyValue(std::string& out, const char* key, const std::string& value) {
    out += key;
    out += '=';
    out += value;
    out += '\n';
}

std::string SerializeCanonicalPreset(const WeatherPresetData& data, bool extendedSliderRange) {
    std::string out;
    out.reserve(768);

    AppendPresetLine(out, kPresetHeader);
    AppendPresetLine(out, "[Meta]");
    AppendPresetKeyValue(out, "FormatVersion", std::to_string(kPresetFormatVersion));
    out += '\n';

    AppendPresetLine(out, "[Weather]");
    AppendPresetKeyValue(out, "ForceClearSky", FormatPresetBool(data.forceClearSky));
    AppendPresetKeyValue(out, "NoRain", FormatPresetBool(data.noRain));
    AppendPresetKeyValue(out, "Rain", FormatPresetFloat(ClampPresetRain(extendedSliderRange, data.rain)));
    AppendPresetKeyValue(out, "Thunder", FormatPresetFloat(ClampPresetThunder(extendedSliderRange, data.thunder)));
    AppendPresetKeyValue(out, "NoDust", FormatPresetBool(data.noDust));
    AppendPresetKeyValue(out, "Dust", FormatPresetFloat(ClampPresetDust(extendedSliderRange, data.dust)));
    AppendPresetKeyValue(out, "NoSnow", FormatPresetBool(data.noSnow));
    AppendPresetKeyValue(out, "Snow", FormatPresetFloat(ClampPresetSnow(extendedSliderRange, data.snow)));
    AppendPresetKeyValue(out, "SnowAccumBoundaryAEnabled", FormatPresetBool(data.snowAccumBoundaryAEnabled));
    AppendPresetKeyValue(out, "SnowAccumBoundaryA", FormatPresetFloat(ClampPresetSnowBoundary(data.snowAccumBoundaryA)));
    AppendPresetKeyValue(out, "SnowAccumBoundaryBEnabled", FormatPresetBool(data.snowAccumBoundaryBEnabled));
    AppendPresetKeyValue(out, "SnowAccumBoundaryB", FormatPresetFloat(ClampPresetSnowBoundary(data.snowAccumBoundaryB)));
    AppendPresetKeyValue(out, "SnowCoverageThresholdEnabled", FormatPresetBool(data.snowCoverageThresholdEnabled));
    AppendPresetKeyValue(out, "SnowCoverageThreshold", FormatPresetFloat(ClampPresetSnowBoundary(data.snowCoverageThreshold)));
    out += '\n';

    AppendPresetLine(out, "[Time]");
    AppendPresetKeyValue(out, "VisualTimeOverride", FormatPresetBool(data.visualTimeOverride));
    AppendPresetKeyValue(out, "ProgressVisualTime", FormatPresetBool(data.visualTimeOverride && data.progressVisualTime));
    AppendPresetKeyValue(out, "ProgressVisualTimeMatchGameTime", FormatPresetBool(data.visualTimeOverride && data.progressVisualTime && data.progressVisualTimeMatchGameTime));
    AppendPresetKeyValue(out, "ProgressVisualTimeIntervalMs", FormatPresetFloat(ClampPresetFloat(data.progressVisualTimeIntervalMs, 0.0f, 5000.0f)));
    AppendPresetKeyValue(out, "TimeHour", FormatPresetFloat(NormalizeHour24(data.timeHour)));
    out += '\n';

    AppendPresetLine(out, "[Cloud]");
    AppendPresetKeyValue(out, "CloudAmountEnabled", FormatPresetBool(data.cloudAmountEnabled));
    AppendPresetKeyValue(out, "CloudAmount", FormatPresetFloat(ClampPresetCloudAmount(extendedSliderRange, data.cloudAmount)));
    AppendPresetKeyValue(out, "CloudHeightEnabled", FormatPresetBool(data.cloudHeightEnabled));
    AppendPresetKeyValue(out, "CloudHeight", FormatPresetFloat(ClampPresetCloudHeight(extendedSliderRange, data.cloudHeight)));
    AppendPresetKeyValue(out, "CloudDensityEnabled", FormatPresetBool(data.cloudDensityEnabled));
    AppendPresetKeyValue(out, "CloudDensity", FormatPresetFloat(ClampPresetCloudDensity(extendedSliderRange, data.cloudDensity)));
    AppendPresetKeyValue(out, "MidCloudsEnabled", FormatPresetBool(data.midCloudsEnabled));
    AppendPresetKeyValue(out, "MidClouds", FormatPresetFloat(ClampPresetCloudWide(extendedSliderRange, data.midClouds)));
    AppendPresetKeyValue(out, "HighCloudLayerEnabled", FormatPresetBool(data.highCloudsEnabled));
    AppendPresetKeyValue(out, "HighCloudLayer", FormatPresetFloat(ClampPresetCloudWide(extendedSliderRange, data.highClouds)));
    AppendPresetKeyValue(out, "CloudAlphaEnabled", FormatPresetBool(data.cloudAlphaEnabled));
    AppendPresetKeyValue(out, "CloudAlpha", FormatPresetFloat(ClampPresetCloudAlpha(extendedSliderRange, data.cloudAlpha)));
    AppendPresetKeyValue(out, "CloudFadeRangeEnabled", FormatPresetBool(data.cloudFadeRangeEnabled));
    AppendPresetKeyValue(out, "CloudFadeRange", FormatPresetFloat(ClampPresetCloudFadeRange(extendedSliderRange, data.cloudFadeRange)));
    AppendPresetKeyValue(out, "CloudDetailRatioEnabled", FormatPresetBool(data.cloudDetailRatioEnabled));
    AppendPresetKeyValue(out, "CloudDetailRatio", FormatPresetFloat(ClampPresetCloudDetailRatio(data.cloudDetailRatio)));
    AppendPresetKeyValue(out, "CloudPhaseFrontEnabled", FormatPresetBool(data.cloudPhaseFrontEnabled));
    AppendPresetKeyValue(out, "CloudPhaseFront", FormatPresetFloat(ClampPresetCloudPhaseFront(extendedSliderRange, data.cloudPhaseFront)));
    AppendPresetKeyValue(out, "CloudScatteringCoefficientEnabled", FormatPresetBool(data.cloudScatteringCoefficientEnabled));
    AppendPresetKeyValue(out, "CloudScatteringCoefficient", FormatPresetFloat(ClampPresetCloudScatteringCoefficient(extendedSliderRange, data.cloudScatteringCoefficient)));
    AppendPresetKeyValue(out, "CloudFlowEnabled", FormatPresetBool(data.cloudFlowEnabled));
    AppendPresetKeyValue(out, "CloudFlow", FormatPresetFloat(ClampPresetCloudFlow(extendedSliderRange, data.cloudFlow)));
    AppendPresetKeyValue(out, "CloudVisibleRangeEnabled", FormatPresetBool(data.cloudVisibleRangeEnabled));
    AppendPresetKeyValue(out, "CloudVisibleRange", FormatPresetFloat(ClampPresetCloudVisibleRange(data.cloudVisibleRange)));
    AppendPresetKeyValue(out, "RayleighHeightEnabled", FormatPresetBool(data.rayleighHeightEnabled));
    AppendPresetKeyValue(out, "RayleighHeight", FormatPresetFloat(ClampPresetRayleighHeight(extendedSliderRange, data.rayleighHeight)));
    AppendPresetKeyValue(out, "OzoneRatioEnabled", FormatPresetBool(data.ozoneRatioEnabled));
    AppendPresetKeyValue(out, "OzoneRatio", FormatPresetFloat(ClampPresetOzoneRatio(extendedSliderRange, data.ozoneRatio)));
    AppendPresetKeyValue(out, "RayleighScatteringColorEnabled", FormatPresetBool(data.rayleighScatteringColorEnabled));
    const WeatherPresetColor rayleigh = ClampPresetColor(data.rayleighScatteringColor, false);
    AppendPresetKeyValue(out, "RayleighScatteringColorR", FormatPresetFloat(rayleigh.r));
    AppendPresetKeyValue(out, "RayleighScatteringColorG", FormatPresetFloat(rayleigh.g));
    AppendPresetKeyValue(out, "RayleighScatteringColorB", FormatPresetFloat(rayleigh.b));
    out += '\n';

    AppendPresetLine(out, "[Experiment]");
    AppendPresetKeyValue(out, "2CEnabled", FormatPresetBool(data.exp2CEnabled));
    AppendPresetKeyValue(out, "2C", FormatPresetFloat(ClampPresetCloudWide(extendedSliderRange, data.exp2C)));
    AppendPresetKeyValue(out, "2DEnabled", FormatPresetBool(data.exp2DEnabled));
    AppendPresetKeyValue(out, "2D", FormatPresetFloat(ClampPresetCloudWide(extendedSliderRange, data.exp2D)));
    AppendPresetKeyValue(out, "CloudVariationEnabled", FormatPresetBool(data.cloudVariationEnabled));
    AppendPresetKeyValue(out, "CloudVariation", FormatPresetFloat(ClampPresetCloudWide(extendedSliderRange, data.cloudVariation)));
    AppendPresetKeyValue(out, "PuddleScaleEnabled", FormatPresetBool(data.puddleScaleEnabled));
    AppendPresetKeyValue(out, "PuddleScale", FormatPresetFloat(ClampPresetPuddleScale(extendedSliderRange, data.puddleScale)));
    out += '\n';

    AppendPresetLine(out, "[Celestial]");
    AppendPresetKeyValue(out, "NightSkyTiltEnabled", FormatPresetBool(data.nightSkyRotationEnabled));
    AppendPresetKeyValue(out, "NightSkyTilt", FormatPresetFloat(ClampPresetPitch(extendedSliderRange, data.nightSkyRotation)));
    AppendPresetKeyValue(out, "NightSkyPhaseEnabled", FormatPresetBool(data.nightSkyYawEnabled));
    AppendPresetKeyValue(out, "NightSkyPhase", FormatPresetFloat(ClampPresetYaw(extendedSliderRange, data.nightSkyYaw)));
    AppendPresetKeyValue(out, "SunSizeEnabled", FormatPresetBool(data.sunSizeEnabled));
    AppendPresetKeyValue(out, "SunSize", FormatPresetFloat(ClampPresetSunSize(extendedSliderRange, data.sunSize)));
    AppendPresetKeyValue(out, "SunLightIntensityEnabled", FormatPresetBool(data.sunLightIntensityEnabled));
    AppendPresetKeyValue(out, "SunLightIntensity", FormatPresetFloat(ClampPresetLightIntensity(extendedSliderRange, data.sunLightIntensity)));
    AppendPresetKeyValue(out, "SunYawEnabled", FormatPresetBool(data.sunYawEnabled));
    AppendPresetKeyValue(out, "SunYaw", FormatPresetFloat(ClampPresetYaw(extendedSliderRange, data.sunYaw)));
    AppendPresetKeyValue(out, "SunPitchEnabled", FormatPresetBool(data.sunPitchEnabled));
    AppendPresetKeyValue(out, "SunPitch", FormatPresetFloat(ClampPresetPitch(extendedSliderRange, data.sunPitch)));
    AppendPresetKeyValue(out, "MoonSizeEnabled", FormatPresetBool(data.moonSizeEnabled));
    AppendPresetKeyValue(out, "MoonSize", FormatPresetFloat(ClampPresetMoonSize(extendedSliderRange, data.moonSize)));
    AppendPresetKeyValue(out, "MoonLightIntensityEnabled", FormatPresetBool(data.moonLightIntensityEnabled));
    AppendPresetKeyValue(out, "MoonLightIntensity", FormatPresetFloat(ClampPresetLightIntensity(extendedSliderRange, data.moonLightIntensity)));
    AppendPresetKeyValue(out, "MoonYawEnabled", FormatPresetBool(data.moonYawEnabled));
    AppendPresetKeyValue(out, "MoonYaw", FormatPresetFloat(ClampPresetYaw(extendedSliderRange, data.moonYaw)));
    AppendPresetKeyValue(out, "MoonPitchEnabled", FormatPresetBool(data.moonPitchEnabled));
    AppendPresetKeyValue(out, "MoonPitch", FormatPresetFloat(ClampPresetPitch(extendedSliderRange, data.moonPitch)));
    AppendPresetKeyValue(out, "MoonRollEnabled", FormatPresetBool(data.moonRollEnabled));
    AppendPresetKeyValue(out, "MoonRoll", FormatPresetFloat(ClampPresetYaw(extendedSliderRange, data.moonRoll)));
    AppendPresetKeyValue(out, "MoonTextureEnabled", FormatPresetBool(data.moonTextureEnabled && !data.moonTexture.empty()));
    AppendPresetKeyValue(out, "MoonTexture", data.moonTextureEnabled ? FormatPresetString(data.moonTexture) : "");
    AppendPresetKeyValue(out, "MilkywayTextureEnabled", FormatPresetBool(data.milkywayTextureEnabled && !data.milkywayTexture.empty()));
    AppendPresetKeyValue(out, "MilkywayTexture", data.milkywayTextureEnabled ? FormatPresetString(data.milkywayTexture) : "");
    out += '\n';

    AppendPresetLine(out, "[Atmosphere]");
    AppendPresetKeyValue(out, "FogEnabled", FormatPresetBool(data.fogEnabled));
    AppendPresetKeyValue(out, "Fog", FormatPresetFloat(ClampPresetFogPercent(extendedSliderRange, data.fogPercent)));
    AppendPresetKeyValue(out, "NativeFogEnabled", FormatPresetBool(data.nativeFogEnabled));
    AppendPresetKeyValue(out, "NativeFog", FormatPresetFloat(ClampPresetNativeFog(extendedSliderRange, data.nativeFog)));
    AppendPresetKeyValue(out, "VolumeFogScatterColorEnabled", FormatPresetBool(data.volumeFogScatterColorEnabled));
    const WeatherPresetColor volumeFog = ClampPresetColor(data.volumeFogScatterColor, true);
    AppendPresetKeyValue(out, "VolumeFogScatterColorR", FormatPresetFloat(volumeFog.r));
    AppendPresetKeyValue(out, "VolumeFogScatterColorG", FormatPresetFloat(volumeFog.g));
    AppendPresetKeyValue(out, "VolumeFogScatterColorB", FormatPresetFloat(volumeFog.b));
    AppendPresetKeyValue(out, "VolumeFogScatterColorA", FormatPresetFloat(volumeFog.a));
    AppendPresetKeyValue(out, "MieScatterColorEnabled", FormatPresetBool(data.mieScatterColorEnabled));
    const WeatherPresetColor mieScatter = ClampPresetColor(data.mieScatterColor, true);
    AppendPresetKeyValue(out, "MieScatterColorR", FormatPresetFloat(mieScatter.r));
    AppendPresetKeyValue(out, "MieScatterColorG", FormatPresetFloat(mieScatter.g));
    AppendPresetKeyValue(out, "MieScatterColorB", FormatPresetFloat(mieScatter.b));
    AppendPresetKeyValue(out, "MieScatterColorA", FormatPresetFloat(mieScatter.a));
    AppendPresetKeyValue(out, "MieScaleHeightEnabled", FormatPresetBool(data.mieScaleHeightEnabled));
    AppendPresetKeyValue(out, "MieScaleHeight", FormatPresetFloat(ClampPresetMieScaleHeight(extendedSliderRange, data.mieScaleHeight)));
    AppendPresetKeyValue(out, "MieAerosolDensityEnabled", FormatPresetBool(data.mieAerosolDensityEnabled));
    AppendPresetKeyValue(out, "MieAerosolDensity", FormatPresetFloat(ClampPresetMieDensity(extendedSliderRange, data.mieAerosolDensity)));
    AppendPresetKeyValue(out, "MieAerosolAbsorptionEnabled", FormatPresetBool(data.mieAerosolAbsorptionEnabled));
    AppendPresetKeyValue(out, "MieAerosolAbsorption", FormatPresetFloat(ClampPresetMieAbsorption(extendedSliderRange, data.mieAerosolAbsorption)));
    AppendPresetKeyValue(out, "HeightFogBaselineEnabled", FormatPresetBool(data.heightFogBaselineEnabled));
    AppendPresetKeyValue(out, "HeightFogBaseline", FormatPresetFloat(ClampPresetHeightFogBaseline(extendedSliderRange, data.heightFogBaseline)));
    AppendPresetKeyValue(out, "HeightFogFalloffEnabled", FormatPresetBool(data.heightFogFalloffEnabled));
    AppendPresetKeyValue(out, "HeightFogFalloff", FormatPresetFloat(ClampPresetHeightFogFalloff(extendedSliderRange, data.heightFogFalloff)));
    AppendPresetKeyValue(out, "NoFog", FormatPresetBool(data.noFog));
    AppendPresetKeyValue(out, "Wind", FormatPresetFloat(ClampPresetWind(extendedSliderRange, data.wind)));
    AppendPresetKeyValue(out, "NoWind", FormatPresetBool(data.noWind));
    if (data.renodxAuroraRegionMaskEnabled) {
        out += '\n';
        AppendPresetLine(out, "[RenoDX]");
        AppendPresetKeyValue(out, "AuroraEnabled", FormatPresetBool(data.renodxAuroraGateEnabled));
        AppendPresetKeyValue(out, "AuroraRegionMask", std::to_string(data.renodxAuroraRegionMask & 126u));
    }

    return out;
}

void AppendRegionSectionHeader(std::string& out, int regionId, const char* section) {
    out += "[Region.";
    out += RegionToken(regionId);
    out += ".";
    out += section;
    out += "]\n";
}

void AppendMaskedRegionPresetData(std::string& out, int regionId, const WeatherPresetData& data, const WeatherPresetMask& mask, bool extendedSliderRange) {
    if (mask.forceClearSky || mask.noRain || mask.rain || mask.thunder || mask.noDust || mask.dust || mask.noSnow || mask.snow ||
        mask.snowAccumBoundaryA || mask.snowAccumBoundaryB || mask.snowCoverageThreshold) {
        AppendRegionSectionHeader(out, regionId, "Weather");
        if (mask.forceClearSky) AppendPresetKeyValue(out, "ForceClearSky", FormatPresetBool(data.forceClearSky));
        if (mask.noRain) AppendPresetKeyValue(out, "NoRain", FormatPresetBool(data.noRain));
        if (mask.rain) AppendPresetKeyValue(out, "Rain", FormatPresetFloat(ClampPresetRain(extendedSliderRange, data.rain)));
        if (mask.thunder) AppendPresetKeyValue(out, "Thunder", FormatPresetFloat(ClampPresetThunder(extendedSliderRange, data.thunder)));
        if (mask.noDust) AppendPresetKeyValue(out, "NoDust", FormatPresetBool(data.noDust));
        if (mask.dust) AppendPresetKeyValue(out, "Dust", FormatPresetFloat(ClampPresetDust(extendedSliderRange, data.dust)));
        if (mask.noSnow) AppendPresetKeyValue(out, "NoSnow", FormatPresetBool(data.noSnow));
        if (mask.snow) AppendPresetKeyValue(out, "Snow", FormatPresetFloat(ClampPresetSnow(extendedSliderRange, data.snow)));
        if (mask.snowAccumBoundaryA) {
            AppendPresetKeyValue(out, "SnowAccumBoundaryAEnabled", FormatPresetBool(data.snowAccumBoundaryAEnabled));
            AppendPresetKeyValue(out, "SnowAccumBoundaryA", FormatPresetFloat(ClampPresetSnowBoundary(data.snowAccumBoundaryA)));
        }
        if (mask.snowAccumBoundaryB) {
            AppendPresetKeyValue(out, "SnowAccumBoundaryBEnabled", FormatPresetBool(data.snowAccumBoundaryBEnabled));
            AppendPresetKeyValue(out, "SnowAccumBoundaryB", FormatPresetFloat(ClampPresetSnowBoundary(data.snowAccumBoundaryB)));
        }
        if (mask.snowCoverageThreshold) {
            AppendPresetKeyValue(out, "SnowCoverageThresholdEnabled", FormatPresetBool(data.snowCoverageThresholdEnabled));
            AppendPresetKeyValue(out, "SnowCoverageThreshold", FormatPresetFloat(ClampPresetSnowBoundary(data.snowCoverageThreshold)));
        }
        out += '\n';
    }

    if (mask.time) {
        AppendRegionSectionHeader(out, regionId, "Time");
        AppendPresetKeyValue(out, "VisualTimeOverride", FormatPresetBool(data.visualTimeOverride));
        AppendPresetKeyValue(out, "ProgressVisualTime", FormatPresetBool(data.visualTimeOverride && data.progressVisualTime));
        AppendPresetKeyValue(out, "ProgressVisualTimeMatchGameTime", FormatPresetBool(data.visualTimeOverride && data.progressVisualTime && data.progressVisualTimeMatchGameTime));
        AppendPresetKeyValue(out, "ProgressVisualTimeIntervalMs", FormatPresetFloat(ClampPresetFloat(data.progressVisualTimeIntervalMs, 0.0f, 5000.0f)));
        AppendPresetKeyValue(out, "TimeHour", FormatPresetFloat(NormalizeHour24(data.timeHour)));
        out += '\n';
    }

    if (mask.cloudAmount || mask.cloudHeight || mask.cloudDensity || mask.midClouds || mask.highClouds ||
        mask.cloudAlpha || mask.cloudFadeRange || mask.cloudDetailRatio ||
        mask.cloudPhaseFront || mask.cloudScatteringCoefficient || mask.cloudFlow || mask.cloudVisibleRange ||
        mask.rayleighHeight || mask.ozoneRatio || mask.rayleighScatteringColor) {
        AppendRegionSectionHeader(out, regionId, "Cloud");
        if (mask.cloudAmount) {
            AppendPresetKeyValue(out, "CloudAmountEnabled", FormatPresetBool(data.cloudAmountEnabled));
            AppendPresetKeyValue(out, "CloudAmount", FormatPresetFloat(ClampPresetCloudAmount(extendedSliderRange, data.cloudAmount)));
        }
        if (mask.cloudHeight) {
            AppendPresetKeyValue(out, "CloudHeightEnabled", FormatPresetBool(data.cloudHeightEnabled));
            AppendPresetKeyValue(out, "CloudHeight", FormatPresetFloat(ClampPresetCloudHeight(extendedSliderRange, data.cloudHeight)));
        }
        if (mask.cloudDensity) {
            AppendPresetKeyValue(out, "CloudDensityEnabled", FormatPresetBool(data.cloudDensityEnabled));
            AppendPresetKeyValue(out, "CloudDensity", FormatPresetFloat(ClampPresetCloudDensity(extendedSliderRange, data.cloudDensity)));
        }
        if (mask.midClouds) {
            AppendPresetKeyValue(out, "MidCloudsEnabled", FormatPresetBool(data.midCloudsEnabled));
            AppendPresetKeyValue(out, "MidClouds", FormatPresetFloat(ClampPresetCloudWide(extendedSliderRange, data.midClouds)));
        }
        if (mask.highClouds) {
            AppendPresetKeyValue(out, "HighCloudLayerEnabled", FormatPresetBool(data.highCloudsEnabled));
            AppendPresetKeyValue(out, "HighCloudLayer", FormatPresetFloat(ClampPresetCloudWide(extendedSliderRange, data.highClouds)));
        }
        if (mask.cloudAlpha) {
            AppendPresetKeyValue(out, "CloudAlphaEnabled", FormatPresetBool(data.cloudAlphaEnabled));
            AppendPresetKeyValue(out, "CloudAlpha", FormatPresetFloat(ClampPresetCloudAlpha(extendedSliderRange, data.cloudAlpha)));
        }
        if (mask.cloudFadeRange) {
            AppendPresetKeyValue(out, "CloudFadeRangeEnabled", FormatPresetBool(data.cloudFadeRangeEnabled));
            AppendPresetKeyValue(out, "CloudFadeRange", FormatPresetFloat(ClampPresetCloudFadeRange(extendedSliderRange, data.cloudFadeRange)));
        }
        if (mask.cloudDetailRatio) {
            AppendPresetKeyValue(out, "CloudDetailRatioEnabled", FormatPresetBool(data.cloudDetailRatioEnabled));
            AppendPresetKeyValue(out, "CloudDetailRatio", FormatPresetFloat(ClampPresetCloudDetailRatio(data.cloudDetailRatio)));
        }
        if (mask.cloudPhaseFront) {
            AppendPresetKeyValue(out, "CloudPhaseFrontEnabled", FormatPresetBool(data.cloudPhaseFrontEnabled));
            AppendPresetKeyValue(out, "CloudPhaseFront", FormatPresetFloat(ClampPresetCloudPhaseFront(extendedSliderRange, data.cloudPhaseFront)));
        }
        if (mask.cloudScatteringCoefficient) {
            AppendPresetKeyValue(out, "CloudScatteringCoefficientEnabled", FormatPresetBool(data.cloudScatteringCoefficientEnabled));
            AppendPresetKeyValue(out, "CloudScatteringCoefficient", FormatPresetFloat(ClampPresetCloudScatteringCoefficient(extendedSliderRange, data.cloudScatteringCoefficient)));
        }
        if (mask.cloudFlow) {
            AppendPresetKeyValue(out, "CloudFlowEnabled", FormatPresetBool(data.cloudFlowEnabled));
            AppendPresetKeyValue(out, "CloudFlow", FormatPresetFloat(ClampPresetCloudFlow(extendedSliderRange, data.cloudFlow)));
        }
        if (mask.cloudVisibleRange) {
            AppendPresetKeyValue(out, "CloudVisibleRangeEnabled", FormatPresetBool(data.cloudVisibleRangeEnabled));
            AppendPresetKeyValue(out, "CloudVisibleRange", FormatPresetFloat(ClampPresetCloudVisibleRange(data.cloudVisibleRange)));
        }
        if (mask.rayleighHeight) {
            AppendPresetKeyValue(out, "RayleighHeightEnabled", FormatPresetBool(data.rayleighHeightEnabled));
            AppendPresetKeyValue(out, "RayleighHeight", FormatPresetFloat(ClampPresetRayleighHeight(extendedSliderRange, data.rayleighHeight)));
        }
        if (mask.ozoneRatio) {
            AppendPresetKeyValue(out, "OzoneRatioEnabled", FormatPresetBool(data.ozoneRatioEnabled));
            AppendPresetKeyValue(out, "OzoneRatio", FormatPresetFloat(ClampPresetOzoneRatio(extendedSliderRange, data.ozoneRatio)));
        }
        if (mask.rayleighScatteringColor) {
            const WeatherPresetColor rayleigh = ClampPresetColor(data.rayleighScatteringColor, false);
            AppendPresetKeyValue(out, "RayleighScatteringColorEnabled", FormatPresetBool(data.rayleighScatteringColorEnabled));
            AppendPresetKeyValue(out, "RayleighScatteringColorR", FormatPresetFloat(rayleigh.r));
            AppendPresetKeyValue(out, "RayleighScatteringColorG", FormatPresetFloat(rayleigh.g));
            AppendPresetKeyValue(out, "RayleighScatteringColorB", FormatPresetFloat(rayleigh.b));
        }
        out += '\n';
    }

    if (mask.exp2C || mask.exp2D || mask.cloudVariation || mask.puddleScale) {
        AppendRegionSectionHeader(out, regionId, "Experiment");
        if (mask.exp2C) {
            AppendPresetKeyValue(out, "2CEnabled", FormatPresetBool(data.exp2CEnabled));
            AppendPresetKeyValue(out, "2C", FormatPresetFloat(ClampPresetCloudWide(extendedSliderRange, data.exp2C)));
        }
        if (mask.exp2D) {
            AppendPresetKeyValue(out, "2DEnabled", FormatPresetBool(data.exp2DEnabled));
            AppendPresetKeyValue(out, "2D", FormatPresetFloat(ClampPresetCloudWide(extendedSliderRange, data.exp2D)));
        }
        if (mask.cloudVariation) {
            AppendPresetKeyValue(out, "CloudVariationEnabled", FormatPresetBool(data.cloudVariationEnabled));
            AppendPresetKeyValue(out, "CloudVariation", FormatPresetFloat(ClampPresetCloudWide(extendedSliderRange, data.cloudVariation)));
        }
        if (mask.puddleScale) {
            AppendPresetKeyValue(out, "PuddleScaleEnabled", FormatPresetBool(data.puddleScaleEnabled));
            AppendPresetKeyValue(out, "PuddleScale", FormatPresetFloat(ClampPresetPuddleScale(extendedSliderRange, data.puddleScale)));
        }
        out += '\n';
    }

    if (mask.nightSkyRotation || mask.nightSkyYaw || mask.sunSize || mask.sunLightIntensity || mask.sunYaw || mask.sunPitch ||
        mask.moonSize || mask.moonLightIntensity || mask.moonYaw || mask.moonPitch || mask.moonRoll || mask.moonTexture || mask.milkywayTexture) {
        AppendRegionSectionHeader(out, regionId, "Celestial");
        if (mask.nightSkyRotation) {
            AppendPresetKeyValue(out, "NightSkyTiltEnabled", FormatPresetBool(data.nightSkyRotationEnabled));
            AppendPresetKeyValue(out, "NightSkyTilt", FormatPresetFloat(ClampPresetPitch(extendedSliderRange, data.nightSkyRotation)));
        }
        if (mask.nightSkyYaw) {
            AppendPresetKeyValue(out, "NightSkyPhaseEnabled", FormatPresetBool(data.nightSkyYawEnabled));
            AppendPresetKeyValue(out, "NightSkyPhase", FormatPresetFloat(ClampPresetYaw(extendedSliderRange, data.nightSkyYaw)));
        }
        if (mask.sunSize) {
            AppendPresetKeyValue(out, "SunSizeEnabled", FormatPresetBool(data.sunSizeEnabled));
            AppendPresetKeyValue(out, "SunSize", FormatPresetFloat(ClampPresetSunSize(extendedSliderRange, data.sunSize)));
        }
        if (mask.sunLightIntensity) {
            AppendPresetKeyValue(out, "SunLightIntensityEnabled", FormatPresetBool(data.sunLightIntensityEnabled));
            AppendPresetKeyValue(out, "SunLightIntensity", FormatPresetFloat(ClampPresetLightIntensity(extendedSliderRange, data.sunLightIntensity)));
        }
        if (mask.sunYaw) {
            AppendPresetKeyValue(out, "SunYawEnabled", FormatPresetBool(data.sunYawEnabled));
            AppendPresetKeyValue(out, "SunYaw", FormatPresetFloat(ClampPresetYaw(extendedSliderRange, data.sunYaw)));
        }
        if (mask.sunPitch) {
            AppendPresetKeyValue(out, "SunPitchEnabled", FormatPresetBool(data.sunPitchEnabled));
            AppendPresetKeyValue(out, "SunPitch", FormatPresetFloat(ClampPresetPitch(extendedSliderRange, data.sunPitch)));
        }
        if (mask.moonSize) {
            AppendPresetKeyValue(out, "MoonSizeEnabled", FormatPresetBool(data.moonSizeEnabled));
            AppendPresetKeyValue(out, "MoonSize", FormatPresetFloat(ClampPresetMoonSize(extendedSliderRange, data.moonSize)));
        }
        if (mask.moonLightIntensity) {
            AppendPresetKeyValue(out, "MoonLightIntensityEnabled", FormatPresetBool(data.moonLightIntensityEnabled));
            AppendPresetKeyValue(out, "MoonLightIntensity", FormatPresetFloat(ClampPresetLightIntensity(extendedSliderRange, data.moonLightIntensity)));
        }
        if (mask.moonYaw) {
            AppendPresetKeyValue(out, "MoonYawEnabled", FormatPresetBool(data.moonYawEnabled));
            AppendPresetKeyValue(out, "MoonYaw", FormatPresetFloat(ClampPresetYaw(extendedSliderRange, data.moonYaw)));
        }
        if (mask.moonPitch) {
            AppendPresetKeyValue(out, "MoonPitchEnabled", FormatPresetBool(data.moonPitchEnabled));
            AppendPresetKeyValue(out, "MoonPitch", FormatPresetFloat(ClampPresetPitch(extendedSliderRange, data.moonPitch)));
        }
        if (mask.moonRoll) {
            AppendPresetKeyValue(out, "MoonRollEnabled", FormatPresetBool(data.moonRollEnabled));
            AppendPresetKeyValue(out, "MoonRoll", FormatPresetFloat(ClampPresetYaw(extendedSliderRange, data.moonRoll)));
        }
        if (mask.moonTexture) {
            AppendPresetKeyValue(out, "MoonTextureEnabled", FormatPresetBool(data.moonTextureEnabled && !data.moonTexture.empty()));
            AppendPresetKeyValue(out, "MoonTexture", data.moonTextureEnabled ? FormatPresetString(data.moonTexture) : "");
        }
        if (mask.milkywayTexture) {
            AppendPresetKeyValue(out, "MilkywayTextureEnabled", FormatPresetBool(data.milkywayTextureEnabled && !data.milkywayTexture.empty()));
            AppendPresetKeyValue(out, "MilkywayTexture", data.milkywayTextureEnabled ? FormatPresetString(data.milkywayTexture) : "");
        }
        out += '\n';
    }

    if (mask.fog || mask.nativeFog || mask.volumeFogScatterColor || mask.mieScatterColor || mask.mieScaleHeight || mask.mieAerosolDensity ||
        mask.mieAerosolAbsorption || mask.heightFogBaseline || mask.heightFogFalloff || mask.noFog || mask.wind || mask.noWind) {
        AppendRegionSectionHeader(out, regionId, "Atmosphere");
        if (mask.fog) {
            AppendPresetKeyValue(out, "FogEnabled", FormatPresetBool(data.fogEnabled));
            AppendPresetKeyValue(out, "Fog", FormatPresetFloat(ClampPresetFogPercent(extendedSliderRange, data.fogPercent)));
        }
        if (mask.nativeFog) {
            AppendPresetKeyValue(out, "NativeFogEnabled", FormatPresetBool(data.nativeFogEnabled));
            AppendPresetKeyValue(out, "NativeFog", FormatPresetFloat(ClampPresetNativeFog(extendedSliderRange, data.nativeFog)));
        }
        if (mask.volumeFogScatterColor) {
            const WeatherPresetColor volumeFog = ClampPresetColor(data.volumeFogScatterColor, true);
            AppendPresetKeyValue(out, "VolumeFogScatterColorEnabled", FormatPresetBool(data.volumeFogScatterColorEnabled));
            AppendPresetKeyValue(out, "VolumeFogScatterColorR", FormatPresetFloat(volumeFog.r));
            AppendPresetKeyValue(out, "VolumeFogScatterColorG", FormatPresetFloat(volumeFog.g));
            AppendPresetKeyValue(out, "VolumeFogScatterColorB", FormatPresetFloat(volumeFog.b));
            AppendPresetKeyValue(out, "VolumeFogScatterColorA", FormatPresetFloat(volumeFog.a));
        }
        if (mask.mieScatterColor) {
            const WeatherPresetColor mieScatter = ClampPresetColor(data.mieScatterColor, true);
            AppendPresetKeyValue(out, "MieScatterColorEnabled", FormatPresetBool(data.mieScatterColorEnabled));
            AppendPresetKeyValue(out, "MieScatterColorR", FormatPresetFloat(mieScatter.r));
            AppendPresetKeyValue(out, "MieScatterColorG", FormatPresetFloat(mieScatter.g));
            AppendPresetKeyValue(out, "MieScatterColorB", FormatPresetFloat(mieScatter.b));
            AppendPresetKeyValue(out, "MieScatterColorA", FormatPresetFloat(mieScatter.a));
        }
        if (mask.mieScaleHeight) {
            AppendPresetKeyValue(out, "MieScaleHeightEnabled", FormatPresetBool(data.mieScaleHeightEnabled));
            AppendPresetKeyValue(out, "MieScaleHeight", FormatPresetFloat(ClampPresetMieScaleHeight(extendedSliderRange, data.mieScaleHeight)));
        }
        if (mask.mieAerosolDensity) {
            AppendPresetKeyValue(out, "MieAerosolDensityEnabled", FormatPresetBool(data.mieAerosolDensityEnabled));
            AppendPresetKeyValue(out, "MieAerosolDensity", FormatPresetFloat(ClampPresetMieDensity(extendedSliderRange, data.mieAerosolDensity)));
        }
        if (mask.mieAerosolAbsorption) {
            AppendPresetKeyValue(out, "MieAerosolAbsorptionEnabled", FormatPresetBool(data.mieAerosolAbsorptionEnabled));
            AppendPresetKeyValue(out, "MieAerosolAbsorption", FormatPresetFloat(ClampPresetMieAbsorption(extendedSliderRange, data.mieAerosolAbsorption)));
        }
        if (mask.heightFogBaseline) {
            AppendPresetKeyValue(out, "HeightFogBaselineEnabled", FormatPresetBool(data.heightFogBaselineEnabled));
            AppendPresetKeyValue(out, "HeightFogBaseline", FormatPresetFloat(ClampPresetHeightFogBaseline(extendedSliderRange, data.heightFogBaseline)));
        }
        if (mask.heightFogFalloff) {
            AppendPresetKeyValue(out, "HeightFogFalloffEnabled", FormatPresetBool(data.heightFogFalloffEnabled));
            AppendPresetKeyValue(out, "HeightFogFalloff", FormatPresetFloat(ClampPresetHeightFogFalloff(extendedSliderRange, data.heightFogFalloff)));
        }
        if (mask.noFog) AppendPresetKeyValue(out, "NoFog", FormatPresetBool(data.noFog));
        if (mask.wind) AppendPresetKeyValue(out, "Wind", FormatPresetFloat(ClampPresetWind(extendedSliderRange, data.wind)));
        if (mask.noWind) AppendPresetKeyValue(out, "NoWind", FormatPresetBool(data.noWind));
        out += '\n';
    }
}

std::string SerializePresetPackage(const WeatherPresetPackage& package, const PresetFormatOptions& options) {
    std::string out = SerializeCanonicalPreset(package.global, options.extendedSliderRange);
    for (int regionId = 1; regionId < kPresetRegionCount; ++regionId) {
        if (!package.regionEnabled[regionId]) continue;
        out += "\n[Region.";
        out += RegionToken(regionId);
        out += "]\n";
        AppendPresetKeyValue(out, "Enabled", "1");
        out += '\n';
        AppendMaskedRegionPresetData(out, regionId, package.region[regionId], package.regionMask[regionId], options.extendedSliderRange);
    }
    return out;
}

bool ReadPresetHeaderLine(const char* path, std::string& outLine) {
    outLine.clear();
    if (!path || !path[0]) return false;
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) return false;
    char line[64] = {};
    const char* raw = fgets(line, static_cast<int>(sizeof(line)), fp);
    fclose(fp);
    if (!raw) return false;
    outLine = line;
    StripUtf8Bom(outLine);
    outLine = TrimCopy(outLine);
    return !outLine.empty();
}

bool IsValidPresetFile(const char* path) {
    std::string firstLine;
    if (!ReadPresetHeaderLine(path, firstLine)) return false;
    return firstLine == kPresetHeader;
}

bool LoadPresetPackageInternal(const char* path, const PresetFormatOptions& options, WeatherPresetPackage& outPackage) {
    if (!IsValidPresetFile(path)) return false;
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) return false;

    PresetParseState globalState{};
    std::array<PresetParseState, kPresetRegionCount> regionStates{};
    std::array<bool, kPresetRegionCount> regionSeen{};
    std::array<bool, kPresetRegionCount> regionEnabled{};
    int currentRegion = kPresetRegionGlobal;
    char line[256] = {};
    bool headerSeen = false;
    bool formatVersionSeen = false;
    int loadedFormatVersion = 0;
    while (fgets(line, static_cast<int>(sizeof(line)), fp)) {
        std::string text = TrimCopy(line);
        StripUtf8Bom(text);
        if (text.empty() || text[0] == ';' || text[0] == '#') continue;
        if (text == kPresetHeader) {
            headerSeen = true;
            continue;
        }
        if (!headerSeen) continue;
        if (!text.empty() && text.front() == '[' && text.back() == ']') {
            currentRegion = kPresetRegionGlobal;
            const std::string section = text.substr(1, text.size() - 2);
            if (section.size() > 7 && _strnicmp(section.c_str(), "Region.", 7) == 0) {
                const size_t tokenStart = 7;
                const size_t tokenEnd = section.find('.', tokenStart);
                const std::string token = tokenEnd == std::string::npos
                    ? section.substr(tokenStart)
                    : section.substr(tokenStart, tokenEnd - tokenStart);
                const int regionId = RegionIdFromToken(token);
                if (regionId > kPresetRegionGlobal && regionId < kPresetRegionCount) {
                    currentRegion = regionId;
                    regionSeen[regionId] = true;
                    regionEnabled[regionId] = true;
                }
            }
            continue;
        }
        if (!text.empty() && text.front() == '[') continue;

        const size_t eq = text.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = TrimCopy(text.substr(0, eq));
        const std::string value = TrimCopy(text.substr(eq + 1));
        if (currentRegion == kPresetRegionGlobal && KeyEquals(key, "FormatVersion")) {
            formatVersionSeen = true;
            loadedFormatVersion = atoi(value.c_str());
            continue;
        }
        if (currentRegion > kPresetRegionGlobal && KeyEquals(key, "Enabled")) {
            bool enabled = true;
            if (TryParseBool(value, enabled)) {
                regionEnabled[currentRegion] = enabled;
            }
            continue;
        }

        if (currentRegion > kPresetRegionGlobal) {
            ParsePresetKeyValue(key, value, regionStates[currentRegion]);
        } else {
            ParsePresetKeyValue(key, value, globalState);
        }
    }

    fclose(fp);
    if (!headerSeen) return false;
    if (!formatVersionSeen) {
        Log("[preset] %s has no FormatVersion; loading as legacy format\n", path);
    } else if (loadedFormatVersion > 0 && loadedFormatVersion < kPresetFormatVersion) {
        Log("[preset] %s format %d loaded by current format %d\n", path, loadedFormatVersion, kPresetFormatVersion);
    } else if (loadedFormatVersion > kPresetFormatVersion) {
        Log("[W] %s format %d is newer than current format %d\n", path, loadedFormatVersion, kPresetFormatVersion);
    }
    WeatherPresetPackage package{};
    NormalizeLoadedPreset(globalState, path, options.extendedSliderRange);
    package.global = globalState.data;
    for (int regionId = 1; regionId < kPresetRegionCount; ++regionId) {
        if (!regionSeen[regionId] || !regionEnabled[regionId]) continue;
        NormalizeLoadedPreset(regionStates[regionId], path, options.extendedSliderRange);
        package.region[regionId] = regionStates[regionId].data;
        package.regionMask[regionId] = regionStates[regionId].mask;
        package.regionEnabled[regionId] = PresetMaskAny(package.regionMask[regionId]);
    }
    outPackage = package;
    return true;
}

bool WritePresetPackageInternal(const char* path, const PresetFormatOptions& options, const WeatherPresetPackage& package) {
    const std::string serialized = SerializePresetPackage(package, options);

    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || !fp) return false;
    const size_t bytesWritten = fwrite(serialized.data(), 1, serialized.size(), fp);
    const bool ok = bytesWritten == serialized.size() && ferror(fp) == 0;
    fclose(fp);
    return ok;
}

bool WritePresetPackageWithCommunityMetadata(
    const char* path,
    const PresetFormatOptions& options,
    const WeatherPresetPackage& package,
    const char* catalogId,
    const char* sha256,
    const char* updatedAt) {
    std::string serialized = SerializePresetPackage(package, options);
    serialized += "\n";
    serialized += "; CrimsonWeatherCommunityId=" + FormatCommunityMetadataString(catalogId) + "\n";
    serialized += "; CrimsonWeatherCommunitySha256=" + FormatCommunityMetadataString(sha256, 80) + "\n";
    serialized += "; CrimsonWeatherCommunityUpdatedAt=" + FormatCommunityMetadataString(updatedAt, 80) + "\n";

    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || !fp) return false;
    const size_t bytesWritten = fwrite(serialized.data(), 1, serialized.size(), fp);
    const bool ok = bytesWritten == serialized.size() && ferror(fp) == 0;
    fclose(fp);
    return ok;
}

bool ReadCommunityMetadataFromPresetFile(const char* path, CommunityPresetInstallInfo& outInfo) {
    outInfo = CommunityPresetInstallInfo{};
    if (!path || !path[0]) return false;

    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) return false;

    char line[256] = {};
    while (fgets(line, static_cast<int>(sizeof(line)), fp)) {
        std::string text = TrimCopy(line);
        if (text.empty()) continue;
        if (text[0] != ';' && text[0] != '#') continue;
        text = TrimCopy(text.substr(1));
        const size_t eq = text.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = TrimCopy(text.substr(0, eq));
        const std::string value = TrimCopy(text.substr(eq + 1));
        if (KeyEquals(key, "CrimsonWeatherCommunityId")) {
            outInfo.catalogId = value;
        } else if (KeyEquals(key, "CrimsonWeatherCommunitySha256")) {
            outInfo.sha256 = value;
        } else if (KeyEquals(key, "CrimsonWeatherCommunityUpdatedAt")) {
            outInfo.updatedAt = value;
        }
    }

    fclose(fp);
    outInfo.valid = !outInfo.catalogId.empty();
    return outInfo.valid;
}

} // namespace preset_internal
