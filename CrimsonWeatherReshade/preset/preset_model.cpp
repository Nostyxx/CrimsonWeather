#include "pch.h"
#include "runtime_shared.h"
#include "preset_model.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace preset_internal {

float ClampPresetFloat(float value, float lo, float hi) {
    return min(hi, max(lo, value));
}

float ClampExtendedPresetFloat(bool extendedSliderRange, float value, float normalLo, float normalHi, float extendedLo, float extendedHi) {
    return extendedSliderRange
        ? ClampPresetFloat(value, extendedLo, extendedHi)
        : ClampPresetFloat(value, normalLo, normalHi);
}

float ClampPresetRain(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 1.0f, 0.0f, 5.0f); }
float ClampPresetThunder(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 1.0f, 0.0f, 5.0f); }
float ClampPresetDust(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 2.0f, 0.0f, 10.0f); }
float ClampPresetSnow(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 1.0f, 0.0f, 5.0f); }
float ClampPresetSnowBoundary(float value) { return ClampPresetFloat(value, -1000.0f, 1500.0f); }
float ClampPresetCloudAmount(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 15.0f, 0.0f, 50.0f); }
float ClampPresetCloudHeight(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, -15.0f, 15.0f, -50.0f, 50.0f); }
float ClampPresetCloudDensity(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 10.0f, 0.0f, 50.0f); }
float ClampPresetCloudWide(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 15.0f, 0.0f, 50.0f); }
float ClampPresetFogPercent(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 100.0f, 0.0f, 500.0f); }
float ClampPresetNativeFog(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 15.0f, 0.0f, 50.0f); }
float ClampPresetWind(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 15.0f, 0.0f, 50.0f); }
float ClampPresetPuddleScale(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 1.0f, 0.0f, 5.0f); }
float ClampPresetPitch(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, -89.0f, 89.0f, -180.0f, 180.0f); }
float ClampPresetYaw(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, -180.0f, 180.0f, -360.0f, 360.0f); }
float ClampPresetSunSize(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.01f, 10.0f, 0.001f, 100.0f); }
float ClampPresetMoonSize(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.020f, 20.0f, 0.001f, 100.0f); }
float ClampPresetLightIntensity(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 20.0f, 0.0f, 100.0f); }
float ClampPresetMieScaleHeight(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 10.0f, 20000.0f, 1.0f, 200000.0f); }
float ClampPresetMieDensity(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 20.0f, 0.0f, 100.0f); }
float ClampPresetMieAbsorption(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 5.0f, 0.0f, 100.0f); }
float ClampPresetHeightFogBaseline(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, -5000.0f, 5000.0f, -50000.0f, 50000.0f); }
float ClampPresetHeightFogFalloff(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 5.0f, 0.0f, 100.0f); }
float ClampPresetCloudAlpha(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 50.0f, 0.0f, 100.0f); }
float ClampPresetCloudFadeRange(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 100000.0f, 0.0f, 200000.0f); }
float ClampPresetCloudDetailRatio(float value) { return ClampPresetFloat(value, 0.0f, 1.5f); }
float ClampPresetCloudPhaseFront(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, -1.0f, 1.0f, -1.0f, 1.0f); }
float ClampPresetCloudScatteringCoefficient(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, kCloudScatteringCoefficientMin, 1.0f, kCloudScatteringCoefficientMin, 100.0f); }
float ClampPresetCloudFlow(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 10.0f, 0.0f, 50.0f); }
float ClampPresetCloudVisibleRange(float value) { return ClampPresetFloat(value, 0.0f, 10.0f); }
float ClampPresetRayleighHeight(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 10.0f, 20000.0f, 1.0f, 200000.0f); }
float ClampPresetOzoneRatio(bool extendedSliderRange, float value) { return ClampExtendedPresetFloat(extendedSliderRange, value, 0.0f, 10.0f, 0.0f, 100.0f); }
float ClampPresetColorComponent(float value) { return ClampPresetFloat(value, 0.0f, 10.0f); }

WeatherPresetColor ClampPresetColor(WeatherPresetColor color, bool includeAlpha) {
    color.r = ClampPresetColorComponent(color.r);
    color.g = ClampPresetColorComponent(color.g);
    color.b = ClampPresetColorComponent(color.b);
    color.a = includeAlpha ? ClampPresetColorComponent(color.a) : 1.0f;
    return color;
}

bool FloatNearlyEqual(float a, float b, float epsilon) {
    return std::fabs(a - b) <= epsilon;
}

bool HourNearlyEqual(float a, float b, float epsilon) {
    const float na = NormalizeHour24(a);
    const float nb = NormalizeHour24(b);
    float delta = std::fabs(na - nb);
    if (delta > 12.0f) delta = 24.0f - delta;
    return delta <= epsilon;
}

bool EnabledFloatNearlyEqual(bool aEnabled, float a, bool bEnabled, float b) {
    if (aEnabled != bEnabled) {
        return false;
    }
    return !aEnabled || FloatNearlyEqual(a, b);
}

bool TimePresetNearlyEqual(const WeatherPresetData& a, const WeatherPresetData& b) {
    if (a.visualTimeOverride != b.visualTimeOverride) {
        return false;
    }
    if (!a.visualTimeOverride) {
        return true;
    }
    if (a.progressVisualTime != b.progressVisualTime) {
        return false;
    }
    if (a.progressVisualTime && a.progressVisualTimeMatchGameTime != b.progressVisualTimeMatchGameTime) {
        return false;
    }
    if (!HourNearlyEqual(a.timeHour, b.timeHour)) {
        return false;
    }
    if (!a.progressVisualTime) {
        return true;
    }
    return FloatNearlyEqual(a.progressVisualTimeIntervalMs, b.progressVisualTimeIntervalMs);
}

bool EnabledStringEquals(bool aEnabled, const std::string& a, bool bEnabled, const std::string& b) {
    if (aEnabled != bEnabled) {
        return false;
    }
    return !aEnabled || EqualsNoCase(a, b);
}

bool ColorNearlyEqual(const WeatherPresetColor& a, const WeatherPresetColor& b, bool includeAlpha) {
    return FloatNearlyEqual(a.r, b.r) &&
        FloatNearlyEqual(a.g, b.g) &&
        FloatNearlyEqual(a.b, b.b) &&
        (!includeAlpha || FloatNearlyEqual(a.a, b.a));
}

bool EnabledColorNearlyEqual(bool aEnabled, const WeatherPresetColor& a, bool bEnabled, const WeatherPresetColor& b, bool includeAlpha) {
    if (aEnabled != bEnabled) {
        return false;
    }
    return !aEnabled || ColorNearlyEqual(a, b, includeAlpha);
}

bool PresetDataEquals(const WeatherPresetData& a, const WeatherPresetData& b) {
    return a.forceClearSky == b.forceClearSky &&
        a.noRain == b.noRain &&
        FloatNearlyEqual(a.rain, b.rain) &&
        FloatNearlyEqual(a.thunder, b.thunder) &&
        a.noDust == b.noDust &&
        FloatNearlyEqual(a.dust, b.dust) &&
        a.noSnow == b.noSnow &&
        FloatNearlyEqual(a.snow, b.snow) &&
        EnabledFloatNearlyEqual(a.snowAccumBoundaryAEnabled, a.snowAccumBoundaryA, b.snowAccumBoundaryAEnabled, b.snowAccumBoundaryA) &&
        EnabledFloatNearlyEqual(a.snowAccumBoundaryBEnabled, a.snowAccumBoundaryB, b.snowAccumBoundaryBEnabled, b.snowAccumBoundaryB) &&
        EnabledFloatNearlyEqual(a.snowCoverageThresholdEnabled, a.snowCoverageThreshold, b.snowCoverageThresholdEnabled, b.snowCoverageThreshold) &&
        TimePresetNearlyEqual(a, b) &&
        EnabledFloatNearlyEqual(a.cloudAmountEnabled, a.cloudAmount, b.cloudAmountEnabled, b.cloudAmount) &&
        EnabledFloatNearlyEqual(a.cloudHeightEnabled, a.cloudHeight, b.cloudHeightEnabled, b.cloudHeight) &&
        EnabledFloatNearlyEqual(a.cloudDensityEnabled, a.cloudDensity, b.cloudDensityEnabled, b.cloudDensity) &&
        EnabledFloatNearlyEqual(a.midCloudsEnabled, a.midClouds, b.midCloudsEnabled, b.midClouds) &&
        EnabledFloatNearlyEqual(a.highCloudsEnabled, a.highClouds, b.highCloudsEnabled, b.highClouds) &&
        EnabledFloatNearlyEqual(a.cloudAlphaEnabled, a.cloudAlpha, b.cloudAlphaEnabled, b.cloudAlpha) &&
        EnabledFloatNearlyEqual(a.cloudFadeRangeEnabled, a.cloudFadeRange, b.cloudFadeRangeEnabled, b.cloudFadeRange) &&
        EnabledFloatNearlyEqual(a.cloudDetailRatioEnabled, a.cloudDetailRatio, b.cloudDetailRatioEnabled, b.cloudDetailRatio) &&
        EnabledFloatNearlyEqual(a.cloudPhaseFrontEnabled, a.cloudPhaseFront, b.cloudPhaseFrontEnabled, b.cloudPhaseFront) &&
        EnabledFloatNearlyEqual(a.cloudScatteringCoefficientEnabled, a.cloudScatteringCoefficient, b.cloudScatteringCoefficientEnabled, b.cloudScatteringCoefficient) &&
        EnabledFloatNearlyEqual(a.cloudFlowEnabled, a.cloudFlow, b.cloudFlowEnabled, b.cloudFlow) &&
        EnabledFloatNearlyEqual(a.cloudVisibleRangeEnabled, a.cloudVisibleRange, b.cloudVisibleRangeEnabled, b.cloudVisibleRange) &&
        EnabledFloatNearlyEqual(a.rayleighHeightEnabled, a.rayleighHeight, b.rayleighHeightEnabled, b.rayleighHeight) &&
        EnabledFloatNearlyEqual(a.ozoneRatioEnabled, a.ozoneRatio, b.ozoneRatioEnabled, b.ozoneRatio) &&
        EnabledColorNearlyEqual(a.rayleighScatteringColorEnabled, a.rayleighScatteringColor, b.rayleighScatteringColorEnabled, b.rayleighScatteringColor, false) &&
        EnabledFloatNearlyEqual(a.exp2CEnabled, a.exp2C, b.exp2CEnabled, b.exp2C) &&
        EnabledFloatNearlyEqual(a.exp2DEnabled, a.exp2D, b.exp2DEnabled, b.exp2D) &&
        EnabledFloatNearlyEqual(a.cloudVariationEnabled, a.cloudVariation, b.cloudVariationEnabled, b.cloudVariation) &&
        EnabledFloatNearlyEqual(a.nightSkyRotationEnabled, a.nightSkyRotation, b.nightSkyRotationEnabled, b.nightSkyRotation) &&
        EnabledFloatNearlyEqual(a.nightSkyYawEnabled, a.nightSkyYaw, b.nightSkyYawEnabled, b.nightSkyYaw) &&
        EnabledFloatNearlyEqual(a.sunSizeEnabled, a.sunSize, b.sunSizeEnabled, b.sunSize) &&
        EnabledFloatNearlyEqual(a.sunLightIntensityEnabled, a.sunLightIntensity, b.sunLightIntensityEnabled, b.sunLightIntensity) &&
        EnabledFloatNearlyEqual(a.sunYawEnabled, a.sunYaw, b.sunYawEnabled, b.sunYaw) &&
        EnabledFloatNearlyEqual(a.sunPitchEnabled, a.sunPitch, b.sunPitchEnabled, b.sunPitch) &&
        EnabledFloatNearlyEqual(a.moonSizeEnabled, a.moonSize, b.moonSizeEnabled, b.moonSize) &&
        EnabledFloatNearlyEqual(a.moonLightIntensityEnabled, a.moonLightIntensity, b.moonLightIntensityEnabled, b.moonLightIntensity) &&
        EnabledFloatNearlyEqual(a.moonYawEnabled, a.moonYaw, b.moonYawEnabled, b.moonYaw) &&
        EnabledFloatNearlyEqual(a.moonPitchEnabled, a.moonPitch, b.moonPitchEnabled, b.moonPitch) &&
        EnabledFloatNearlyEqual(a.moonRollEnabled, a.moonRoll, b.moonRollEnabled, b.moonRoll) &&
        EnabledStringEquals(a.moonTextureEnabled, a.moonTexture, b.moonTextureEnabled, b.moonTexture) &&
        EnabledStringEquals(a.milkywayTextureEnabled, a.milkywayTexture, b.milkywayTextureEnabled, b.milkywayTexture) &&
        EnabledFloatNearlyEqual(a.fogEnabled, a.fogPercent, b.fogEnabled, b.fogPercent) &&
        EnabledFloatNearlyEqual(a.nativeFogEnabled, a.nativeFog, b.nativeFogEnabled, b.nativeFog) &&
        EnabledColorNearlyEqual(a.volumeFogScatterColorEnabled, a.volumeFogScatterColor, b.volumeFogScatterColorEnabled, b.volumeFogScatterColor, true) &&
        EnabledColorNearlyEqual(a.mieScatterColorEnabled, a.mieScatterColor, b.mieScatterColorEnabled, b.mieScatterColor, true) &&
        EnabledFloatNearlyEqual(a.mieScaleHeightEnabled, a.mieScaleHeight, b.mieScaleHeightEnabled, b.mieScaleHeight) &&
        EnabledFloatNearlyEqual(a.mieAerosolDensityEnabled, a.mieAerosolDensity, b.mieAerosolDensityEnabled, b.mieAerosolDensity) &&
        EnabledFloatNearlyEqual(a.mieAerosolAbsorptionEnabled, a.mieAerosolAbsorption, b.mieAerosolAbsorptionEnabled, b.mieAerosolAbsorption) &&
        EnabledFloatNearlyEqual(a.heightFogBaselineEnabled, a.heightFogBaseline, b.heightFogBaselineEnabled, b.heightFogBaseline) &&
        EnabledFloatNearlyEqual(a.heightFogFalloffEnabled, a.heightFogFalloff, b.heightFogFalloffEnabled, b.heightFogFalloff) &&
        a.noFog == b.noFog &&
        FloatNearlyEqual(a.wind, b.wind) &&
        a.noWind == b.noWind &&
        EnabledFloatNearlyEqual(a.puddleScaleEnabled, a.puddleScale, b.puddleScaleEnabled, b.puddleScale);
}

bool PresetMaskAny(const WeatherPresetMask& mask) {
    return mask.forceClearSky || mask.noRain || mask.rain || mask.thunder || mask.noDust || mask.dust || mask.noSnow || mask.snow ||
        mask.snowAccumBoundaryA || mask.snowAccumBoundaryB || mask.snowCoverageThreshold || mask.time ||
        mask.cloudAmount || mask.cloudHeight || mask.cloudDensity || mask.midClouds ||
        mask.highClouds || mask.cloudAlpha || mask.cloudFadeRange || mask.cloudDetailRatio ||
        mask.cloudPhaseFront || mask.cloudScatteringCoefficient || mask.cloudFlow || mask.cloudVisibleRange ||
        mask.rayleighHeight || mask.ozoneRatio || mask.rayleighScatteringColor ||
        mask.exp2C || mask.exp2D || mask.cloudVariation ||
        mask.nightSkyRotation || mask.nightSkyYaw || mask.sunSize || mask.sunLightIntensity || mask.sunYaw || mask.sunPitch ||
        mask.moonSize || mask.moonLightIntensity || mask.moonYaw || mask.moonPitch || mask.moonRoll || mask.moonTexture || mask.milkywayTexture ||
        mask.fog || mask.nativeFog || mask.volumeFogScatterColor || mask.mieScatterColor || mask.mieScaleHeight || mask.mieAerosolDensity ||
        mask.mieAerosolAbsorption || mask.heightFogBaseline || mask.heightFogFalloff || mask.noFog || mask.wind ||
        mask.noWind || mask.puddleScale;
}

WeatherPresetSourceMask ToSourceMask(const WeatherPresetMask& mask) {
    WeatherPresetSourceMask out{};
    out.forceClearSky = mask.forceClearSky;
    out.noRain = mask.noRain;
    out.rain = mask.rain;
    out.thunder = mask.thunder;
    out.noDust = mask.noDust;
    out.dust = mask.dust;
    out.noSnow = mask.noSnow;
    out.snow = mask.snow;
    out.snowAccumBoundaryA = mask.snowAccumBoundaryA;
    out.snowAccumBoundaryB = mask.snowAccumBoundaryB;
    out.snowCoverageThreshold = mask.snowCoverageThreshold;
    out.time = mask.time;
    out.cloudAmount = mask.cloudAmount;
    out.cloudHeight = mask.cloudHeight;
    out.cloudDensity = mask.cloudDensity;
    out.midClouds = mask.midClouds;
    out.highClouds = mask.highClouds;
    out.cloudAlpha = mask.cloudAlpha;
    out.cloudFadeRange = mask.cloudFadeRange;
    out.cloudDetailRatio = mask.cloudDetailRatio;
    out.cloudPhaseFront = mask.cloudPhaseFront;
    out.cloudScatteringCoefficient = mask.cloudScatteringCoefficient;
    out.cloudFlow = mask.cloudFlow;
    out.cloudVisibleRange = mask.cloudVisibleRange;
    out.rayleighHeight = mask.rayleighHeight;
    out.ozoneRatio = mask.ozoneRatio;
    out.rayleighScatteringColor = mask.rayleighScatteringColor;
    out.exp2C = mask.exp2C;
    out.exp2D = mask.exp2D;
    out.cloudVariation = mask.cloudVariation;
    out.nightSkyRotation = mask.nightSkyRotation;
    out.nightSkyYaw = mask.nightSkyYaw;
    out.sunSize = mask.sunSize;
    out.sunLightIntensity = mask.sunLightIntensity;
    out.sunYaw = mask.sunYaw;
    out.sunPitch = mask.sunPitch;
    out.moonSize = mask.moonSize;
    out.moonLightIntensity = mask.moonLightIntensity;
    out.moonYaw = mask.moonYaw;
    out.moonPitch = mask.moonPitch;
    out.moonRoll = mask.moonRoll;
    out.moonTexture = mask.moonTexture;
    out.milkywayTexture = mask.milkywayTexture;
    out.fog = mask.fog;
    out.nativeFog = mask.nativeFog;
    out.volumeFogScatterColor = mask.volumeFogScatterColor;
    out.mieScatterColor = mask.mieScatterColor;
    out.mieScaleHeight = mask.mieScaleHeight;
    out.mieAerosolDensity = mask.mieAerosolDensity;
    out.mieAerosolAbsorption = mask.mieAerosolAbsorption;
    out.heightFogBaseline = mask.heightFogBaseline;
    out.heightFogFalloff = mask.heightFogFalloff;
    out.noFog = mask.noFog;
    out.wind = mask.wind;
    out.noWind = mask.noWind;
    out.puddleScale = mask.puddleScale;
    return out;
}

WeatherPresetMask FromSourceMask(const WeatherPresetSourceMask& source) {
    WeatherPresetMask mask{};
    mask.forceClearSky = source.forceClearSky;
    mask.noRain = source.noRain;
    mask.rain = source.rain;
    mask.thunder = source.thunder;
    mask.noDust = source.noDust;
    mask.dust = source.dust;
    mask.noSnow = source.noSnow;
    mask.snow = source.snow;
    mask.snowAccumBoundaryA = source.snowAccumBoundaryA;
    mask.snowAccumBoundaryB = source.snowAccumBoundaryB;
    mask.snowCoverageThreshold = source.snowCoverageThreshold;
    mask.time = source.time;
    mask.cloudAmount = source.cloudAmount;
    mask.cloudHeight = source.cloudHeight;
    mask.cloudDensity = source.cloudDensity;
    mask.midClouds = source.midClouds;
    mask.highClouds = source.highClouds;
    mask.cloudAlpha = source.cloudAlpha;
    mask.cloudFadeRange = source.cloudFadeRange;
    mask.cloudDetailRatio = source.cloudDetailRatio;
    mask.cloudPhaseFront = source.cloudPhaseFront;
    mask.cloudScatteringCoefficient = source.cloudScatteringCoefficient;
    mask.cloudFlow = source.cloudFlow;
    mask.cloudVisibleRange = source.cloudVisibleRange;
    mask.rayleighHeight = source.rayleighHeight;
    mask.ozoneRatio = source.ozoneRatio;
    mask.rayleighScatteringColor = source.rayleighScatteringColor;
    mask.exp2C = source.exp2C;
    mask.exp2D = source.exp2D;
    mask.cloudVariation = source.cloudVariation;
    mask.nightSkyRotation = source.nightSkyRotation;
    mask.nightSkyYaw = source.nightSkyYaw;
    mask.sunSize = source.sunSize;
    mask.sunLightIntensity = source.sunLightIntensity;
    mask.sunYaw = source.sunYaw;
    mask.sunPitch = source.sunPitch;
    mask.moonSize = source.moonSize;
    mask.moonLightIntensity = source.moonLightIntensity;
    mask.moonYaw = source.moonYaw;
    mask.moonPitch = source.moonPitch;
    mask.moonRoll = source.moonRoll;
    mask.moonTexture = source.moonTexture;
    mask.milkywayTexture = source.milkywayTexture;
    mask.fog = source.fog;
    mask.nativeFog = source.nativeFog;
    mask.volumeFogScatterColor = source.volumeFogScatterColor;
    mask.mieScatterColor = source.mieScatterColor;
    mask.mieScaleHeight = source.mieScaleHeight;
    mask.mieAerosolDensity = source.mieAerosolDensity;
    mask.mieAerosolAbsorption = source.mieAerosolAbsorption;
    mask.heightFogBaseline = source.heightFogBaseline;
    mask.heightFogFalloff = source.heightFogFalloff;
    mask.noFog = source.noFog;
    mask.wind = source.wind;
    mask.noWind = source.noWind;
    mask.puddleScale = source.puddleScale;
    return mask;
}

bool PresetMaskEquals(const WeatherPresetMask& a, const WeatherPresetMask& b) {
    return a.forceClearSky == b.forceClearSky &&
        a.noRain == b.noRain &&
        a.rain == b.rain &&
        a.thunder == b.thunder &&
        a.noDust == b.noDust &&
        a.dust == b.dust &&
        a.noSnow == b.noSnow &&
        a.snow == b.snow &&
        a.snowAccumBoundaryA == b.snowAccumBoundaryA &&
        a.snowAccumBoundaryB == b.snowAccumBoundaryB &&
        a.snowCoverageThreshold == b.snowCoverageThreshold &&
        a.time == b.time &&
        a.cloudAmount == b.cloudAmount &&
        a.cloudHeight == b.cloudHeight &&
        a.cloudDensity == b.cloudDensity &&
        a.midClouds == b.midClouds &&
        a.highClouds == b.highClouds &&
        a.cloudAlpha == b.cloudAlpha &&
        a.cloudFadeRange == b.cloudFadeRange &&
        a.cloudDetailRatio == b.cloudDetailRatio &&
        a.cloudPhaseFront == b.cloudPhaseFront &&
        a.cloudScatteringCoefficient == b.cloudScatteringCoefficient &&
        a.cloudFlow == b.cloudFlow &&
        a.cloudVisibleRange == b.cloudVisibleRange &&
        a.rayleighHeight == b.rayleighHeight &&
        a.ozoneRatio == b.ozoneRatio &&
        a.rayleighScatteringColor == b.rayleighScatteringColor &&
        a.exp2C == b.exp2C &&
        a.exp2D == b.exp2D &&
        a.cloudVariation == b.cloudVariation &&
        a.nightSkyRotation == b.nightSkyRotation &&
        a.nightSkyYaw == b.nightSkyYaw &&
        a.sunSize == b.sunSize &&
        a.sunLightIntensity == b.sunLightIntensity &&
        a.sunYaw == b.sunYaw &&
        a.sunPitch == b.sunPitch &&
        a.moonSize == b.moonSize &&
        a.moonLightIntensity == b.moonLightIntensity &&
        a.moonYaw == b.moonYaw &&
        a.moonPitch == b.moonPitch &&
        a.moonRoll == b.moonRoll &&
        a.moonTexture == b.moonTexture &&
        a.milkywayTexture == b.milkywayTexture &&
        a.fog == b.fog &&
        a.nativeFog == b.nativeFog &&
        a.volumeFogScatterColor == b.volumeFogScatterColor &&
        a.mieScatterColor == b.mieScatterColor &&
        a.mieScaleHeight == b.mieScaleHeight &&
        a.mieAerosolDensity == b.mieAerosolDensity &&
        a.mieAerosolAbsorption == b.mieAerosolAbsorption &&
        a.heightFogBaseline == b.heightFogBaseline &&
        a.heightFogFalloff == b.heightFogFalloff &&
        a.noFog == b.noFog &&
        a.wind == b.wind &&
        a.noWind == b.noWind &&
        a.puddleScale == b.puddleScale;
}

WeatherPresetMask BuildFullPresetMask() {
    WeatherPresetMask mask{};
    mask.forceClearSky = true;
    mask.noRain = true;
    mask.rain = true;
    mask.thunder = true;
    mask.noDust = true;
    mask.dust = true;
    mask.noSnow = true;
    mask.snow = true;
    mask.snowAccumBoundaryA = true;
    mask.snowAccumBoundaryB = true;
    mask.snowCoverageThreshold = true;
    mask.time = true;
    mask.cloudAmount = true;
    mask.cloudHeight = true;
    mask.cloudDensity = true;
    mask.midClouds = true;
    mask.highClouds = true;
    mask.cloudAlpha = true;
    mask.cloudFadeRange = true;
    mask.cloudDetailRatio = true;
    mask.cloudPhaseFront = true;
    mask.cloudScatteringCoefficient = true;
    mask.cloudFlow = true;
    mask.cloudVisibleRange = true;
    mask.rayleighHeight = true;
    mask.ozoneRatio = true;
    mask.rayleighScatteringColor = true;
    mask.exp2C = true;
    mask.exp2D = true;
    mask.cloudVariation = true;
    mask.nightSkyRotation = true;
    mask.nightSkyYaw = true;
    mask.sunSize = true;
    mask.sunLightIntensity = true;
    mask.sunYaw = true;
    mask.sunPitch = true;
    mask.moonSize = true;
    mask.moonLightIntensity = true;
    mask.moonYaw = true;
    mask.moonPitch = true;
    mask.moonRoll = true;
    mask.moonTexture = true;
    mask.milkywayTexture = true;
    mask.fog = true;
    mask.nativeFog = true;
    mask.volumeFogScatterColor = true;
    mask.mieScatterColor = true;
    mask.mieScaleHeight = true;
    mask.mieAerosolDensity = true;
    mask.mieAerosolAbsorption = true;
    mask.heightFogBaseline = true;
    mask.heightFogFalloff = true;
    mask.noFog = true;
    mask.wind = true;
    mask.noWind = true;
    mask.puddleScale = true;
    return mask;
}

WeatherPresetMask BuildOverrideMask(const WeatherPresetData& base, const WeatherPresetData& value) {
    WeatherPresetMask mask{};
    mask.forceClearSky = base.forceClearSky != value.forceClearSky;
    mask.noRain = base.noRain != value.noRain;
    mask.rain = !FloatNearlyEqual(base.rain, value.rain);
    mask.thunder = !FloatNearlyEqual(base.thunder, value.thunder);
    mask.noDust = base.noDust != value.noDust;
    mask.dust = !FloatNearlyEqual(base.dust, value.dust);
    mask.noSnow = base.noSnow != value.noSnow;
    mask.snow = !FloatNearlyEqual(base.snow, value.snow);
    mask.snowAccumBoundaryA = !EnabledFloatNearlyEqual(base.snowAccumBoundaryAEnabled, base.snowAccumBoundaryA, value.snowAccumBoundaryAEnabled, value.snowAccumBoundaryA);
    mask.snowAccumBoundaryB = !EnabledFloatNearlyEqual(base.snowAccumBoundaryBEnabled, base.snowAccumBoundaryB, value.snowAccumBoundaryBEnabled, value.snowAccumBoundaryB);
    mask.snowCoverageThreshold = !EnabledFloatNearlyEqual(base.snowCoverageThresholdEnabled, base.snowCoverageThreshold, value.snowCoverageThresholdEnabled, value.snowCoverageThreshold);
    mask.time = !TimePresetNearlyEqual(base, value);
    mask.cloudAmount = !EnabledFloatNearlyEqual(base.cloudAmountEnabled, base.cloudAmount, value.cloudAmountEnabled, value.cloudAmount);
    mask.cloudHeight = !EnabledFloatNearlyEqual(base.cloudHeightEnabled, base.cloudHeight, value.cloudHeightEnabled, value.cloudHeight);
    mask.cloudDensity = !EnabledFloatNearlyEqual(base.cloudDensityEnabled, base.cloudDensity, value.cloudDensityEnabled, value.cloudDensity);
    mask.midClouds = !EnabledFloatNearlyEqual(base.midCloudsEnabled, base.midClouds, value.midCloudsEnabled, value.midClouds);
    mask.highClouds = !EnabledFloatNearlyEqual(base.highCloudsEnabled, base.highClouds, value.highCloudsEnabled, value.highClouds);
    mask.cloudAlpha = !EnabledFloatNearlyEqual(base.cloudAlphaEnabled, base.cloudAlpha, value.cloudAlphaEnabled, value.cloudAlpha);
    mask.cloudFadeRange = !EnabledFloatNearlyEqual(base.cloudFadeRangeEnabled, base.cloudFadeRange, value.cloudFadeRangeEnabled, value.cloudFadeRange);
    mask.cloudDetailRatio = !EnabledFloatNearlyEqual(base.cloudDetailRatioEnabled, base.cloudDetailRatio, value.cloudDetailRatioEnabled, value.cloudDetailRatio);
    mask.cloudPhaseFront = !EnabledFloatNearlyEqual(base.cloudPhaseFrontEnabled, base.cloudPhaseFront, value.cloudPhaseFrontEnabled, value.cloudPhaseFront);
    mask.cloudScatteringCoefficient = !EnabledFloatNearlyEqual(base.cloudScatteringCoefficientEnabled, base.cloudScatteringCoefficient, value.cloudScatteringCoefficientEnabled, value.cloudScatteringCoefficient);
    mask.cloudFlow = !EnabledFloatNearlyEqual(base.cloudFlowEnabled, base.cloudFlow, value.cloudFlowEnabled, value.cloudFlow);
    mask.cloudVisibleRange = !EnabledFloatNearlyEqual(base.cloudVisibleRangeEnabled, base.cloudVisibleRange, value.cloudVisibleRangeEnabled, value.cloudVisibleRange);
    mask.rayleighHeight = !EnabledFloatNearlyEqual(base.rayleighHeightEnabled, base.rayleighHeight, value.rayleighHeightEnabled, value.rayleighHeight);
    mask.ozoneRatio = !EnabledFloatNearlyEqual(base.ozoneRatioEnabled, base.ozoneRatio, value.ozoneRatioEnabled, value.ozoneRatio);
    mask.rayleighScatteringColor = !EnabledColorNearlyEqual(base.rayleighScatteringColorEnabled, base.rayleighScatteringColor, value.rayleighScatteringColorEnabled, value.rayleighScatteringColor, false);
    mask.exp2C = !EnabledFloatNearlyEqual(base.exp2CEnabled, base.exp2C, value.exp2CEnabled, value.exp2C);
    mask.exp2D = !EnabledFloatNearlyEqual(base.exp2DEnabled, base.exp2D, value.exp2DEnabled, value.exp2D);
    mask.cloudVariation = !EnabledFloatNearlyEqual(base.cloudVariationEnabled, base.cloudVariation, value.cloudVariationEnabled, value.cloudVariation);
    mask.nightSkyRotation = !EnabledFloatNearlyEqual(base.nightSkyRotationEnabled, base.nightSkyRotation, value.nightSkyRotationEnabled, value.nightSkyRotation);
    mask.nightSkyYaw = !EnabledFloatNearlyEqual(base.nightSkyYawEnabled, base.nightSkyYaw, value.nightSkyYawEnabled, value.nightSkyYaw);
    mask.sunSize = !EnabledFloatNearlyEqual(base.sunSizeEnabled, base.sunSize, value.sunSizeEnabled, value.sunSize);
    mask.sunLightIntensity = !EnabledFloatNearlyEqual(base.sunLightIntensityEnabled, base.sunLightIntensity, value.sunLightIntensityEnabled, value.sunLightIntensity);
    mask.sunYaw = !EnabledFloatNearlyEqual(base.sunYawEnabled, base.sunYaw, value.sunYawEnabled, value.sunYaw);
    mask.sunPitch = !EnabledFloatNearlyEqual(base.sunPitchEnabled, base.sunPitch, value.sunPitchEnabled, value.sunPitch);
    mask.moonSize = !EnabledFloatNearlyEqual(base.moonSizeEnabled, base.moonSize, value.moonSizeEnabled, value.moonSize);
    mask.moonLightIntensity = !EnabledFloatNearlyEqual(base.moonLightIntensityEnabled, base.moonLightIntensity, value.moonLightIntensityEnabled, value.moonLightIntensity);
    mask.moonYaw = !EnabledFloatNearlyEqual(base.moonYawEnabled, base.moonYaw, value.moonYawEnabled, value.moonYaw);
    mask.moonPitch = !EnabledFloatNearlyEqual(base.moonPitchEnabled, base.moonPitch, value.moonPitchEnabled, value.moonPitch);
    mask.moonRoll = !EnabledFloatNearlyEqual(base.moonRollEnabled, base.moonRoll, value.moonRollEnabled, value.moonRoll);
    mask.moonTexture = !EnabledStringEquals(base.moonTextureEnabled, base.moonTexture, value.moonTextureEnabled, value.moonTexture);
    mask.milkywayTexture = !EnabledStringEquals(base.milkywayTextureEnabled, base.milkywayTexture, value.milkywayTextureEnabled, value.milkywayTexture);
    mask.fog = !EnabledFloatNearlyEqual(base.fogEnabled, base.fogPercent, value.fogEnabled, value.fogPercent);
    mask.nativeFog = !EnabledFloatNearlyEqual(base.nativeFogEnabled, base.nativeFog, value.nativeFogEnabled, value.nativeFog);
    mask.volumeFogScatterColor = !EnabledColorNearlyEqual(base.volumeFogScatterColorEnabled, base.volumeFogScatterColor, value.volumeFogScatterColorEnabled, value.volumeFogScatterColor, true);
    mask.mieScatterColor = !EnabledColorNearlyEqual(base.mieScatterColorEnabled, base.mieScatterColor, value.mieScatterColorEnabled, value.mieScatterColor, true);
    mask.mieScaleHeight = !EnabledFloatNearlyEqual(base.mieScaleHeightEnabled, base.mieScaleHeight, value.mieScaleHeightEnabled, value.mieScaleHeight);
    mask.mieAerosolDensity = !EnabledFloatNearlyEqual(base.mieAerosolDensityEnabled, base.mieAerosolDensity, value.mieAerosolDensityEnabled, value.mieAerosolDensity);
    mask.mieAerosolAbsorption = !EnabledFloatNearlyEqual(base.mieAerosolAbsorptionEnabled, base.mieAerosolAbsorption, value.mieAerosolAbsorptionEnabled, value.mieAerosolAbsorption);
    mask.heightFogBaseline = !EnabledFloatNearlyEqual(base.heightFogBaselineEnabled, base.heightFogBaseline, value.heightFogBaselineEnabled, value.heightFogBaseline);
    mask.heightFogFalloff = !EnabledFloatNearlyEqual(base.heightFogFalloffEnabled, base.heightFogFalloff, value.heightFogFalloffEnabled, value.heightFogFalloff);
    mask.noFog = base.noFog != value.noFog;
    mask.wind = !FloatNearlyEqual(base.wind, value.wind);
    mask.noWind = base.noWind != value.noWind;
    mask.puddleScale = !EnabledFloatNearlyEqual(base.puddleScaleEnabled, base.puddleScale, value.puddleScaleEnabled, value.puddleScale);
    return mask;
}

void ApplyPresetMask(WeatherPresetData& target, const WeatherPresetData& source, const WeatherPresetMask& mask) {
    if (mask.forceClearSky) target.forceClearSky = source.forceClearSky;
    if (mask.noRain) target.noRain = source.noRain;
    if (mask.rain) target.rain = source.rain;
    if (mask.thunder) target.thunder = source.thunder;
    if (mask.noDust) target.noDust = source.noDust;
    if (mask.dust) target.dust = source.dust;
    if (mask.noSnow) target.noSnow = source.noSnow;
    if (mask.snow) target.snow = source.snow;
    if (mask.snowAccumBoundaryA) { target.snowAccumBoundaryAEnabled = source.snowAccumBoundaryAEnabled; target.snowAccumBoundaryA = source.snowAccumBoundaryA; }
    if (mask.snowAccumBoundaryB) { target.snowAccumBoundaryBEnabled = source.snowAccumBoundaryBEnabled; target.snowAccumBoundaryB = source.snowAccumBoundaryB; }
    if (mask.snowCoverageThreshold) { target.snowCoverageThresholdEnabled = source.snowCoverageThresholdEnabled; target.snowCoverageThreshold = source.snowCoverageThreshold; }
    if (mask.time) {
        target.visualTimeOverride = source.visualTimeOverride;
        target.progressVisualTime = source.visualTimeOverride && source.progressVisualTime;
        target.progressVisualTimeMatchGameTime = target.progressVisualTime && source.progressVisualTimeMatchGameTime;
        target.progressVisualTimeIntervalMs = source.progressVisualTimeIntervalMs;
        target.timeHour = source.timeHour;
    }
    if (mask.cloudAmount) {
        target.cloudAmountEnabled = source.cloudAmountEnabled;
        target.cloudAmount = source.cloudAmount;
    }
    if (mask.cloudHeight) {
        target.cloudHeightEnabled = source.cloudHeightEnabled;
        target.cloudHeight = source.cloudHeight;
    }
    if (mask.cloudDensity) {
        target.cloudDensityEnabled = source.cloudDensityEnabled;
        target.cloudDensity = source.cloudDensity;
    }
    if (mask.midClouds) {
        target.midCloudsEnabled = source.midCloudsEnabled;
        target.midClouds = source.midClouds;
    }
    if (mask.highClouds) {
        target.highCloudsEnabled = source.highCloudsEnabled;
        target.highClouds = source.highClouds;
    }
    if (mask.cloudAlpha) {
        target.cloudAlphaEnabled = source.cloudAlphaEnabled;
        target.cloudAlpha = source.cloudAlpha;
    }
    if (mask.cloudFadeRange) {
        target.cloudFadeRangeEnabled = source.cloudFadeRangeEnabled;
        target.cloudFadeRange = source.cloudFadeRange;
    }
    if (mask.cloudDetailRatio) {
        target.cloudDetailRatioEnabled = source.cloudDetailRatioEnabled;
        target.cloudDetailRatio = source.cloudDetailRatio;
    }
    if (mask.cloudPhaseFront) {
        target.cloudPhaseFrontEnabled = source.cloudPhaseFrontEnabled;
        target.cloudPhaseFront = source.cloudPhaseFront;
    }
    if (mask.cloudScatteringCoefficient) {
        target.cloudScatteringCoefficientEnabled = source.cloudScatteringCoefficientEnabled;
        target.cloudScatteringCoefficient = source.cloudScatteringCoefficient;
    }
    if (mask.cloudFlow) {
        target.cloudFlowEnabled = source.cloudFlowEnabled;
        target.cloudFlow = source.cloudFlow;
    }
    if (mask.cloudVisibleRange) {
        target.cloudVisibleRangeEnabled = source.cloudVisibleRangeEnabled;
        target.cloudVisibleRange = source.cloudVisibleRange;
    }
    if (mask.rayleighHeight) {
        target.rayleighHeightEnabled = source.rayleighHeightEnabled;
        target.rayleighHeight = source.rayleighHeight;
    }
    if (mask.ozoneRatio) {
        target.ozoneRatioEnabled = source.ozoneRatioEnabled;
        target.ozoneRatio = source.ozoneRatio;
    }
    if (mask.rayleighScatteringColor) {
        target.rayleighScatteringColorEnabled = source.rayleighScatteringColorEnabled;
        target.rayleighScatteringColor = source.rayleighScatteringColor;
    }
    if (mask.exp2C) {
        target.exp2CEnabled = source.exp2CEnabled;
        target.exp2C = source.exp2C;
    }
    if (mask.exp2D) {
        target.exp2DEnabled = source.exp2DEnabled;
        target.exp2D = source.exp2D;
    }
    if (mask.cloudVariation) {
        target.cloudVariationEnabled = source.cloudVariationEnabled;
        target.cloudVariation = source.cloudVariation;
    }
    if (mask.nightSkyRotation) {
        target.nightSkyRotationEnabled = source.nightSkyRotationEnabled;
        target.nightSkyRotation = source.nightSkyRotation;
    }
    if (mask.nightSkyYaw) {
        target.nightSkyYawEnabled = source.nightSkyYawEnabled;
        target.nightSkyYaw = source.nightSkyYaw;
    }
    if (mask.sunSize) {
        target.sunSizeEnabled = source.sunSizeEnabled;
        target.sunSize = source.sunSize;
    }
    if (mask.sunLightIntensity) {
        target.sunLightIntensityEnabled = source.sunLightIntensityEnabled;
        target.sunLightIntensity = source.sunLightIntensity;
    }
    if (mask.sunYaw) {
        target.sunYawEnabled = source.sunYawEnabled;
        target.sunYaw = source.sunYaw;
    }
    if (mask.sunPitch) {
        target.sunPitchEnabled = source.sunPitchEnabled;
        target.sunPitch = source.sunPitch;
    }
    if (mask.moonSize) {
        target.moonSizeEnabled = source.moonSizeEnabled;
        target.moonSize = source.moonSize;
    }
    if (mask.moonLightIntensity) {
        target.moonLightIntensityEnabled = source.moonLightIntensityEnabled;
        target.moonLightIntensity = source.moonLightIntensity;
    }
    if (mask.moonYaw) {
        target.moonYawEnabled = source.moonYawEnabled;
        target.moonYaw = source.moonYaw;
    }
    if (mask.moonPitch) {
        target.moonPitchEnabled = source.moonPitchEnabled;
        target.moonPitch = source.moonPitch;
    }
    if (mask.moonRoll) {
        target.moonRollEnabled = source.moonRollEnabled;
        target.moonRoll = source.moonRoll;
    }
    if (mask.moonTexture) {
        target.moonTextureEnabled = source.moonTextureEnabled;
        target.moonTexture = source.moonTexture;
    }
    if (mask.milkywayTexture) {
        target.milkywayTextureEnabled = source.milkywayTextureEnabled;
        target.milkywayTexture = source.milkywayTexture;
    }
    if (mask.fog) {
        target.fogEnabled = source.fogEnabled;
        target.fogPercent = source.fogPercent;
    }
    if (mask.nativeFog) {
        target.nativeFogEnabled = source.nativeFogEnabled;
        target.nativeFog = source.nativeFog;
    }
    if (mask.volumeFogScatterColor) {
        target.volumeFogScatterColorEnabled = source.volumeFogScatterColorEnabled;
        target.volumeFogScatterColor = source.volumeFogScatterColor;
    }
    if (mask.mieScatterColor) {
        target.mieScatterColorEnabled = source.mieScatterColorEnabled;
        target.mieScatterColor = source.mieScatterColor;
    }
    if (mask.mieScaleHeight) {
        target.mieScaleHeightEnabled = source.mieScaleHeightEnabled;
        target.mieScaleHeight = source.mieScaleHeight;
    }
    if (mask.mieAerosolDensity) {
        target.mieAerosolDensityEnabled = source.mieAerosolDensityEnabled;
        target.mieAerosolDensity = source.mieAerosolDensity;
    }
    if (mask.mieAerosolAbsorption) {
        target.mieAerosolAbsorptionEnabled = source.mieAerosolAbsorptionEnabled;
        target.mieAerosolAbsorption = source.mieAerosolAbsorption;
    }
    if (mask.heightFogBaseline) {
        target.heightFogBaselineEnabled = source.heightFogBaselineEnabled;
        target.heightFogBaseline = source.heightFogBaseline;
    }
    if (mask.heightFogFalloff) {
        target.heightFogFalloffEnabled = source.heightFogFalloffEnabled;
        target.heightFogFalloff = source.heightFogFalloff;
    }
    if (mask.noFog) target.noFog = source.noFog;
    if (mask.wind) target.wind = source.wind;
    if (mask.noWind) target.noWind = source.noWind;
    if (mask.puddleScale) {
        target.puddleScaleEnabled = source.puddleScaleEnabled;
        target.puddleScale = source.puddleScale;
    }
}

float LerpPresetFloat(float a, float b, float t) {
    return a + (b - a) * t;
}

WeatherPresetColor LerpPresetColor(const WeatherPresetColor& a, const WeatherPresetColor& b, float t) {
    return {
        LerpPresetFloat(a.r, b.r, t),
        LerpPresetFloat(a.g, b.g, t),
        LerpPresetFloat(a.b, b.b, t),
        LerpPresetFloat(a.a, b.a, t),
    };
}

float LerpPresetHour(float a, float b, float t) {
    const float na = NormalizeHour24(a);
    const float nb = NormalizeHour24(b);
    float delta = nb - na;
    if (delta > 12.0f) delta -= 24.0f;
    if (delta < -12.0f) delta += 24.0f;
    return NormalizeHour24(na + delta * t);
}

float NormalizeSignedDegrees(float value) {
    while (value > 180.0f) value -= 360.0f;
    while (value < -180.0f) value += 360.0f;
    return value;
}

float LerpPresetDegrees180(float a, float b, float t) {
    float delta = NormalizeSignedDegrees(b - a);
    return NormalizeSignedDegrees(a + delta * t);
}

bool ChoosePresetBool(bool a, bool b, float t) {
    return t >= 0.5f ? b : a;
}

WeatherPresetData BlendPresetData(const WeatherPresetData& a, const WeatherPresetData& b, float t) {
    t = ClampPresetFloat(t, 0.0f, 1.0f);
    if (t <= 0.0001f) return a;
    if (t >= 0.9999f) return b;

    WeatherPresetData out{};
    out.forceClearSky = ChoosePresetBool(a.forceClearSky, b.forceClearSky, t);
    out.noRain = ChoosePresetBool(a.noRain, b.noRain, t);
    out.rain = LerpPresetFloat(a.rain, b.rain, t);
    out.thunder = LerpPresetFloat(a.thunder, b.thunder, t);
    out.noDust = ChoosePresetBool(a.noDust, b.noDust, t);
    out.dust = LerpPresetFloat(a.dust, b.dust, t);
    out.noSnow = ChoosePresetBool(a.noSnow, b.noSnow, t);
    out.snow = LerpPresetFloat(a.snow, b.snow, t);
    out.snowAccumBoundaryAEnabled = a.snowAccumBoundaryAEnabled || b.snowAccumBoundaryAEnabled;
    out.snowAccumBoundaryA = LerpPresetFloat(a.snowAccumBoundaryA, b.snowAccumBoundaryA, t);
    out.snowAccumBoundaryBEnabled = a.snowAccumBoundaryBEnabled || b.snowAccumBoundaryBEnabled;
    out.snowAccumBoundaryB = LerpPresetFloat(a.snowAccumBoundaryB, b.snowAccumBoundaryB, t);
    out.snowCoverageThresholdEnabled = a.snowCoverageThresholdEnabled || b.snowCoverageThresholdEnabled;
    out.snowCoverageThreshold = LerpPresetFloat(a.snowCoverageThreshold, b.snowCoverageThreshold, t);
    out.visualTimeOverride = ChoosePresetBool(a.visualTimeOverride, b.visualTimeOverride, t);
    out.progressVisualTime = out.visualTimeOverride && ChoosePresetBool(a.progressVisualTime, b.progressVisualTime, t);
    out.progressVisualTimeMatchGameTime = out.progressVisualTime && ChoosePresetBool(a.progressVisualTimeMatchGameTime, b.progressVisualTimeMatchGameTime, t);
    out.progressVisualTimeIntervalMs = (t < 0.5f) ? a.progressVisualTimeIntervalMs : b.progressVisualTimeIntervalMs;
    out.timeHour = (a.visualTimeOverride && b.visualTimeOverride)
        ? LerpPresetHour(a.timeHour, b.timeHour, t)
        : (out.visualTimeOverride ? b.timeHour : a.timeHour);
    out.cloudAmountEnabled = a.cloudAmountEnabled || b.cloudAmountEnabled;
    out.cloudAmount = LerpPresetFloat(a.cloudAmount, b.cloudAmount, t);
    out.cloudHeightEnabled = a.cloudHeightEnabled || b.cloudHeightEnabled;
    out.cloudHeight = LerpPresetFloat(a.cloudHeight, b.cloudHeight, t);
    out.cloudDensityEnabled = a.cloudDensityEnabled || b.cloudDensityEnabled;
    out.cloudDensity = LerpPresetFloat(a.cloudDensity, b.cloudDensity, t);
    out.midCloudsEnabled = a.midCloudsEnabled || b.midCloudsEnabled;
    out.midClouds = LerpPresetFloat(a.midClouds, b.midClouds, t);
    out.highCloudsEnabled = a.highCloudsEnabled || b.highCloudsEnabled;
    out.highClouds = LerpPresetFloat(a.highClouds, b.highClouds, t);
    out.cloudAlphaEnabled = a.cloudAlphaEnabled || b.cloudAlphaEnabled;
    out.cloudAlpha = LerpPresetFloat(a.cloudAlpha, b.cloudAlpha, t);
    out.cloudFadeRangeEnabled = a.cloudFadeRangeEnabled || b.cloudFadeRangeEnabled;
    out.cloudFadeRange = LerpPresetFloat(a.cloudFadeRange, b.cloudFadeRange, t);
    out.cloudDetailRatioEnabled = a.cloudDetailRatioEnabled || b.cloudDetailRatioEnabled;
    out.cloudDetailRatio = LerpPresetFloat(a.cloudDetailRatio, b.cloudDetailRatio, t);
    out.cloudPhaseFrontEnabled = a.cloudPhaseFrontEnabled || b.cloudPhaseFrontEnabled;
    out.cloudPhaseFront = LerpPresetFloat(a.cloudPhaseFront, b.cloudPhaseFront, t);
    out.cloudScatteringCoefficientEnabled = a.cloudScatteringCoefficientEnabled || b.cloudScatteringCoefficientEnabled;
    out.cloudScatteringCoefficient = LerpPresetFloat(a.cloudScatteringCoefficient, b.cloudScatteringCoefficient, t);
    out.cloudFlowEnabled = a.cloudFlowEnabled || b.cloudFlowEnabled;
    out.cloudFlow = LerpPresetFloat(a.cloudFlow, b.cloudFlow, t);
    out.cloudVisibleRangeEnabled = a.cloudVisibleRangeEnabled || b.cloudVisibleRangeEnabled;
    out.cloudVisibleRange = LerpPresetFloat(a.cloudVisibleRange, b.cloudVisibleRange, t);
    out.rayleighHeightEnabled = a.rayleighHeightEnabled || b.rayleighHeightEnabled;
    out.rayleighHeight = LerpPresetFloat(a.rayleighHeight, b.rayleighHeight, t);
    out.ozoneRatioEnabled = a.ozoneRatioEnabled || b.ozoneRatioEnabled;
    out.ozoneRatio = LerpPresetFloat(a.ozoneRatio, b.ozoneRatio, t);
    out.rayleighScatteringColorEnabled = a.rayleighScatteringColorEnabled || b.rayleighScatteringColorEnabled;
    out.rayleighScatteringColor = LerpPresetColor(a.rayleighScatteringColor, b.rayleighScatteringColor, t);
    out.exp2CEnabled = a.exp2CEnabled || b.exp2CEnabled;
    out.exp2C = LerpPresetFloat(a.exp2C, b.exp2C, t);
    out.exp2DEnabled = a.exp2DEnabled || b.exp2DEnabled;
    out.exp2D = LerpPresetFloat(a.exp2D, b.exp2D, t);
    out.cloudVariationEnabled = a.cloudVariationEnabled || b.cloudVariationEnabled;
    out.cloudVariation = LerpPresetFloat(a.cloudVariation, b.cloudVariation, t);
    out.nightSkyRotationEnabled = a.nightSkyRotationEnabled || b.nightSkyRotationEnabled;
    out.nightSkyRotation = LerpPresetFloat(a.nightSkyRotation, b.nightSkyRotation, t);
    out.nightSkyYawEnabled = a.nightSkyYawEnabled || b.nightSkyYawEnabled;
    out.nightSkyYaw = LerpPresetDegrees180(a.nightSkyYaw, b.nightSkyYaw, t);
    out.sunSizeEnabled = a.sunSizeEnabled || b.sunSizeEnabled;
    out.sunSize = LerpPresetFloat(a.sunSize, b.sunSize, t);
    out.sunLightIntensityEnabled = a.sunLightIntensityEnabled || b.sunLightIntensityEnabled;
    out.sunLightIntensity = LerpPresetFloat(a.sunLightIntensity, b.sunLightIntensity, t);
    out.sunYawEnabled = a.sunYawEnabled || b.sunYawEnabled;
    out.sunYaw = LerpPresetDegrees180(a.sunYaw, b.sunYaw, t);
    out.sunPitchEnabled = a.sunPitchEnabled || b.sunPitchEnabled;
    out.sunPitch = LerpPresetFloat(a.sunPitch, b.sunPitch, t);
    out.moonSizeEnabled = a.moonSizeEnabled || b.moonSizeEnabled;
    out.moonSize = LerpPresetFloat(a.moonSize, b.moonSize, t);
    out.moonLightIntensityEnabled = a.moonLightIntensityEnabled || b.moonLightIntensityEnabled;
    out.moonLightIntensity = LerpPresetFloat(a.moonLightIntensity, b.moonLightIntensity, t);
    out.moonYawEnabled = a.moonYawEnabled || b.moonYawEnabled;
    out.moonYaw = LerpPresetDegrees180(a.moonYaw, b.moonYaw, t);
    out.moonPitchEnabled = a.moonPitchEnabled || b.moonPitchEnabled;
    out.moonPitch = LerpPresetFloat(a.moonPitch, b.moonPitch, t);
    out.moonRollEnabled = a.moonRollEnabled || b.moonRollEnabled;
    out.moonRoll = LerpPresetDegrees180(a.moonRoll, b.moonRoll, t);
    out.moonTextureEnabled = ChoosePresetBool(a.moonTextureEnabled, b.moonTextureEnabled, t);
    out.moonTexture = t >= 0.5f ? b.moonTexture : a.moonTexture;
    out.milkywayTextureEnabled = ChoosePresetBool(a.milkywayTextureEnabled, b.milkywayTextureEnabled, t);
    out.milkywayTexture = t >= 0.5f ? b.milkywayTexture : a.milkywayTexture;
    out.fogEnabled = a.fogEnabled || b.fogEnabled;
    out.fogPercent = LerpPresetFloat(a.fogPercent, b.fogPercent, t);
    out.nativeFogEnabled = a.nativeFogEnabled || b.nativeFogEnabled;
    out.nativeFog = LerpPresetFloat(a.nativeFog, b.nativeFog, t);
    out.volumeFogScatterColorEnabled = a.volumeFogScatterColorEnabled || b.volumeFogScatterColorEnabled;
    out.volumeFogScatterColor = LerpPresetColor(a.volumeFogScatterColor, b.volumeFogScatterColor, t);
    out.mieScatterColorEnabled = a.mieScatterColorEnabled || b.mieScatterColorEnabled;
    out.mieScatterColor = LerpPresetColor(a.mieScatterColor, b.mieScatterColor, t);
    out.mieScaleHeightEnabled = a.mieScaleHeightEnabled || b.mieScaleHeightEnabled;
    out.mieScaleHeight = LerpPresetFloat(a.mieScaleHeight, b.mieScaleHeight, t);
    out.mieAerosolDensityEnabled = a.mieAerosolDensityEnabled || b.mieAerosolDensityEnabled;
    out.mieAerosolDensity = LerpPresetFloat(a.mieAerosolDensity, b.mieAerosolDensity, t);
    out.mieAerosolAbsorptionEnabled = a.mieAerosolAbsorptionEnabled || b.mieAerosolAbsorptionEnabled;
    out.mieAerosolAbsorption = LerpPresetFloat(a.mieAerosolAbsorption, b.mieAerosolAbsorption, t);
    out.heightFogBaselineEnabled = a.heightFogBaselineEnabled || b.heightFogBaselineEnabled;
    out.heightFogBaseline = LerpPresetFloat(a.heightFogBaseline, b.heightFogBaseline, t);
    out.heightFogFalloffEnabled = a.heightFogFalloffEnabled || b.heightFogFalloffEnabled;
    out.heightFogFalloff = LerpPresetFloat(a.heightFogFalloff, b.heightFogFalloff, t);
    out.noFog = ChoosePresetBool(a.noFog, b.noFog, t);
    out.wind = LerpPresetFloat(a.wind, b.wind, t);
    out.noWind = ChoosePresetBool(a.noWind, b.noWind, t);
    out.puddleScaleEnabled = a.puddleScaleEnabled || b.puddleScaleEnabled;
    out.puddleScale = LerpPresetFloat(a.puddleScale, b.puddleScale, t);
    return out;
}

bool IsPresetRegionId(int regionId) {
    return regionId >= kPresetRegionGlobal && regionId < kPresetRegionCount;
}

const char* RegionToken(int regionId) {
    switch (regionId) {
    case kPresetRegionHernand: return "Hernand";
    case kPresetRegionDemeniss: return "Demeniss";
    case kPresetRegionDelesyia: return "Delesyia";
    case kPresetRegionPailune: return "Pailune";
    case kPresetRegionCrimsonDesert: return "CrimsonDesert";
    case kPresetRegionAbyss: return "Abyss";
    default: return "Global";
    }
}

const char* RegionDisplayName(int regionId) {
    switch (regionId) {
    case kPresetRegionHernand: return "Hernand";
    case kPresetRegionDemeniss: return "Demeniss";
    case kPresetRegionDelesyia: return "Delesyia";
    case kPresetRegionPailune: return "Pailune";
    case kPresetRegionCrimsonDesert: return "Crimson Desert";
    case kPresetRegionAbyss: return "Abyss";
    default: return "Global";
    }
}

int RegionIdFromToken(const std::string& token) {
    if (EqualsNoCase(token, "Hernand")) return kPresetRegionHernand;
    if (EqualsNoCase(token, "Demeniss")) return kPresetRegionDemeniss;
    if (EqualsNoCase(token, "Delesyia")) return kPresetRegionDelesyia;
    if (EqualsNoCase(token, "Pailune")) return kPresetRegionPailune;
    if (EqualsNoCase(token, "CrimsonDesert") || EqualsNoCase(token, "Crimson Desert")) return kPresetRegionCrimsonDesert;
    if (EqualsNoCase(token, "Abyss")) return kPresetRegionAbyss;
    return kPresetRegionGlobal;
}

int CurrentMajorRegionForPreset() {
    const int region = g_regionMajorId.load();
    return IsPresetRegionId(region) ? region : kPresetRegionGlobal;
}

WeatherPresetData EffectivePresetDataForRegion(const WeatherPresetPackage& package, int regionId) {
    WeatherPresetData data = package.global;
    if (regionId > kPresetRegionGlobal && regionId < kPresetRegionCount && package.regionEnabled[regionId]) {
        ApplyPresetMask(data, package.region[regionId], package.regionMask[regionId]);
    }
    return data;
}

bool PresetPackageEquals(const WeatherPresetPackage& a, const WeatherPresetPackage& b) {
    if (!PresetDataEquals(a.global, b.global)) return false;
    for (int regionId = 1; regionId < kPresetRegionCount; ++regionId) {
        if (a.regionEnabled[regionId] != b.regionEnabled[regionId]) return false;
        if (!PresetMaskEquals(a.regionMask[regionId], b.regionMask[regionId])) return false;
        if (!PresetDataEquals(
                EffectivePresetDataForRegion(a, regionId),
                EffectivePresetDataForRegion(b, regionId))) {
            return false;
        }
    }
    return true;
}

std::string TrimCopy(const std::string& value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(start, end - start);
}

void StripUtf8Bom(std::string& value) {
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEF &&
        static_cast<unsigned char>(value[1]) == 0xBB &&
        static_cast<unsigned char>(value[2]) == 0xBF) {
        value.erase(0, 3);
    }
}

bool EqualsNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool EndsWithIni(const std::string& value) {
    if (value.size() < 4) return false;
    return _stricmp(value.c_str() + value.size() - 4, ".ini") == 0;
}

std::string EnsureIniExtension(const std::string& value) {
    if (value.empty()) return value;
    return EndsWithIni(value) ? value : (value + ".ini");
}

std::string GetPresetDisplayNameFromFileName(const std::string& fileName) {
    if (EndsWithIni(fileName)) {
        return fileName.substr(0, fileName.size() - 4);
    }
    return fileName;
}

std::string GetPresetDirectory() {
    if (g_pluginDir[0]) return std::string(g_pluginDir);
    return ".";
}

std::string JoinPath(const std::string& dir, const std::string& fileName) {
    if (dir.empty()) return fileName;
    if (dir.back() == '\\' || dir.back() == '/') return dir + fileName;
    return dir + "\\" + fileName;
}

std::string GetCommunityPresetDirectory() {
    return JoinPath(JoinPath(JoinPath(GetPresetDirectory(), "CrimsonWeather"), "community"), "preset");
}

} // namespace preset_internal
