#include "pch.h"
#include "runtime_shared.h"
#include "preset_service.h"
#include "renodx_bridge.h"
#include "sky_texture_override.h"
#include "preset_model.h"
#include "preset_format.h"
#include "preset_schedule.h"

using namespace preset_internal;

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

bool SelectPresetIndexInternal(int index, bool applyImmediately, const char* statusPrefix, const char* toastPrefix, const char* logVerb);

namespace {
constexpr float kCloudScatteringCoefficientMin = 0.00001f;

std::vector<PresetListItem> g_presetItems;
bool g_presetsInitialized = false;
int g_selectedPresetIndex = -1;
WeatherPresetPackage g_selectedPresetBaseline{};
bool g_selectedPresetBaselineValid = false;
WeatherPresetPackage g_editDraftPackage{};
bool g_editDraftValid = false;
bool g_newPresetDraftActive = false;
int g_presetEditRegion = kPresetRegionGlobal;
int g_lastAppliedRegion = -1;
WeatherPresetData g_lastRuntimeAppliedData{};
bool g_lastRuntimeAppliedDataValid = false;
int g_regionBlendFrom = -1;
int g_regionBlendTo = -1;
int g_regionBlendLogBucket = -1;
bool g_autoSavePending = false;
ULONGLONG g_autoSaveLastEditTick = 0;

constexpr float kPresetFloatEpsilon = 0.0005f;
constexpr float kRegionTransitionDurationSeconds = 6.0f;
constexpr ULONGLONG kAutoSaveDebounceMs = 350;
constexpr bool kPresetVerboseTestLog = false;
constexpr const char* kNewPresetDisplayName = "[New Preset]";
constexpr const char* kPresetConfigSection = "Preset";
constexpr const char* kPresetConfigKeyLastPreset = "LastPreset";
WeatherPresetData CaptureCurrentPresetData();
void ApplyPresetData(const WeatherPresetData& data);
bool SaveSelectedDraftInternal(const char* statusPrefix, const char* logVerb, bool applyAfterSave);

PresetFormatOptions CurrentPresetFormatOptions() {
    return { g_extendedSliderRange.load() };
}

bool HasSelectedPresetIndexInternal() {
    return g_selectedPresetIndex >= 0 && g_selectedPresetIndex < static_cast<int>(g_presetItems.size());
}

void ClearSelectedPresetBaseline() {
    g_selectedPresetBaseline = WeatherPresetPackage{};
    g_selectedPresetBaselineValid = false;
    g_editDraftPackage = WeatherPresetPackage{};
    g_editDraftValid = true;
    g_newPresetDraftActive = false;
    g_lastAppliedRegion = -1;
    g_lastRuntimeAppliedData = WeatherPresetData{};
    g_lastRuntimeAppliedDataValid = false;
    g_regionBlendFrom = -1;
    g_regionBlendTo = -1;
    g_regionBlendLogBucket = -1;
    g_autoSavePending = false;
    g_autoSaveLastEditTick = 0;
}

void SetSelectedPresetBaseline(const WeatherPresetPackage& package) {
    g_selectedPresetBaseline = package;
    g_selectedPresetBaselineValid = true;
    g_editDraftPackage = package;
    g_editDraftValid = true;
    g_newPresetDraftActive = false;
    g_autoSavePending = false;
    g_autoSaveLastEditTick = 0;
}

void AppendMaskField(std::string& out, bool enabled, const char* name) {
    if (!enabled) return;
    if (!out.empty()) out += ",";
    out += name;
}

std::string PresetMaskSummary(const WeatherPresetMask& mask) {
    std::string out;
    AppendMaskField(out, mask.forceClearSky, "clear");
    AppendMaskField(out, mask.noRain, "noRain");
    AppendMaskField(out, mask.rain, "rain");
    AppendMaskField(out, mask.thunder, "thunder");
    AppendMaskField(out, mask.noDust, "noDust");
    AppendMaskField(out, mask.dust, "dust");
    AppendMaskField(out, mask.noSnow, "noSnow");
    AppendMaskField(out, mask.snow, "snow");
    AppendMaskField(out, mask.time, "time");
    AppendMaskField(out, mask.cloudAmount, "cloudAmt");
    AppendMaskField(out, mask.cloudHeight, "cloudH");
    AppendMaskField(out, mask.cloudDensity, "cloudD");
    AppendMaskField(out, mask.midClouds, "midClouds");
    AppendMaskField(out, mask.highClouds, "highClouds");
    AppendMaskField(out, mask.exp2C, "2C");
    AppendMaskField(out, mask.exp2D, "2D");
    AppendMaskField(out, mask.cloudVariation, "cloudVar");
    AppendMaskField(out, mask.nightSkyRotation, "nightTilt");
    AppendMaskField(out, mask.nightSkyYaw, "nightPhase");
    AppendMaskField(out, mask.sunSize, "sunSize");
    AppendMaskField(out, mask.sunYaw, "sunYaw");
    AppendMaskField(out, mask.sunPitch, "sunPitch");
    AppendMaskField(out, mask.moonSize, "moonSize");
    AppendMaskField(out, mask.moonYaw, "moonYaw");
    AppendMaskField(out, mask.moonPitch, "moonPitch");
    AppendMaskField(out, mask.moonRoll, "moonRoll");
    AppendMaskField(out, mask.moonTexture, "moonTexture");
    AppendMaskField(out, mask.milkywayTexture, "milkywayTexture");
    AppendMaskField(out, mask.fog, "fog");
    AppendMaskField(out, mask.nativeFog, "nativeFog");
    AppendMaskField(out, mask.noFog, "noFog");
    AppendMaskField(out, mask.wind, "wind");
    AppendMaskField(out, mask.noWind, "noWind");
    AppendMaskField(out, mask.puddleScale, "puddle");
    return out.empty() ? std::string("<none>") : out;
}

void LogPresetDataSummary(const char* tag, int regionId, const WeatherPresetData& data) {
    if (!kPresetVerboseTestLog) return;
    Log("[preset-test] %s region=%s clear=%d noRain=%d rain=%.3f thunder=%.3f noDust=%d dust=%.3f noSnow=%d snow=%.3f time=%d progress=%d@%.2f/%.0fms wind=%.3f noWind=%d fog=%d/%.1f nativeFog=%d/%.2f noFog=%d cloudAmt=%d/%.2f cloudH=%d/%.2f cloudD=%d/%.2f puddle=%d/%.3f\n",
        tag ? tag : "data",
        RegionDisplayName(regionId),
        data.forceClearSky ? 1 : 0,
        data.noRain ? 1 : 0,
        data.rain,
        data.thunder,
        data.noDust ? 1 : 0,
        data.dust,
        data.noSnow ? 1 : 0,
        data.snow,
        data.visualTimeOverride ? 1 : 0,
        data.progressVisualTime ? 1 : 0,
        NormalizeHour24(data.timeHour),
        ClampPresetFloat(data.progressVisualTimeIntervalMs, 0.0f, 5000.0f),
        data.wind,
        data.noWind ? 1 : 0,
        data.fogEnabled ? 1 : 0,
        data.fogPercent,
        data.nativeFogEnabled ? 1 : 0,
        data.nativeFog,
        data.noFog ? 1 : 0,
        data.cloudAmountEnabled ? 1 : 0,
        data.cloudAmount,
        data.cloudHeightEnabled ? 1 : 0,
        data.cloudHeight,
        data.cloudDensityEnabled ? 1 : 0,
        data.cloudDensity,
        data.puddleScaleEnabled ? 1 : 0,
        data.puddleScale);
}

void LogRegionMaskSummary(const char* tag, int regionId, const WeatherPresetPackage& package) {
    if (!kPresetVerboseTestLog) return;
    if (regionId <= kPresetRegionGlobal || regionId >= kPresetRegionCount) return;
    const std::string mask = package.regionEnabled[regionId]
        ? PresetMaskSummary(package.regionMask[regionId])
        : std::string("<inherits-global>");
    Log("[preset-test] %s region=%s enabled=%d mask=%s\n",
        tag ? tag : "mask",
        RegionDisplayName(regionId),
        package.regionEnabled[regionId] ? 1 : 0,
        mask.c_str());
}

void LogPresetPackageSummary(const char* tag, const WeatherPresetPackage& package) {
    LogPresetDataSummary(tag ? tag : "package-global", kPresetRegionGlobal, package.global);
    for (int regionId = 1; regionId < kPresetRegionCount; ++regionId) {
        if (!package.regionEnabled[regionId]) continue;
        LogRegionMaskSummary("package-region-mask", regionId, package);
        LogPresetDataSummary("package-region-effective", regionId, EffectivePresetDataForRegion(package, regionId));
    }
}

void SaveEditedRegionToPackage(WeatherPresetPackage& package, int regionId, const WeatherPresetData& data) {
    if (regionId <= kPresetRegionGlobal || regionId >= kPresetRegionCount) {
        package.global = data;
        LogPresetDataSummary("edit-global", kPresetRegionGlobal, package.global);
        return;
    }

    const WeatherPresetMask mask = BuildOverrideMask(package.global, data);
    package.region[regionId] = data;
    package.regionMask[regionId] = mask;
    package.regionEnabled[regionId] = PresetMaskAny(mask);
    LogRegionMaskSummary("edit-region-mask", regionId, package);
    LogPresetDataSummary("edit-region-effective", regionId, EffectivePresetDataForRegion(package, regionId));
}

void SaveEditedRegionToPackageWithMask(
    WeatherPresetPackage& package,
    int regionId,
    const WeatherPresetData& data,
    const WeatherPresetMask& mask) {
    if (regionId <= kPresetRegionGlobal || regionId >= kPresetRegionCount) {
        package.global = data;
        return;
    }

    package.region[regionId] = data;
    package.regionMask[regionId] = mask;
    package.regionEnabled[regionId] = PresetMaskAny(mask);
    LogRegionMaskSummary("edit-region-mask", regionId, package);
    LogPresetDataSummary("edit-region-effective", regionId, EffectivePresetDataForRegion(package, regionId));
}

void ResetRegionToDefaultsInPackage(WeatherPresetPackage& package, int regionId) {
    if (regionId <= kPresetRegionGlobal || regionId >= kPresetRegionCount) return;
    package.region[regionId] = WeatherPresetData{};
    package.regionMask[regionId] = WeatherPresetMask{};
    package.regionEnabled[regionId] = false;
    Log("[preset] cleared %s region overrides; now inherits Global\n", RegionDisplayName(regionId));
    LogRegionMaskSummary("reset-region-mask", regionId, package);
    LogPresetDataSummary("reset-region-effective", regionId, EffectivePresetDataForRegion(package, regionId));
}

void EnsureEditDraft() {
    if (g_editDraftValid) return;
    g_editDraftPackage = g_selectedPresetBaselineValid ? g_selectedPresetBaseline : WeatherPresetPackage{};
    g_editDraftValid = true;
}

bool EditRegionAffectsCurrentRuntime() {
    if (!IsPresetRegionId(g_presetEditRegion)) return false;
    if (g_presetEditRegion == kPresetRegionGlobal) return true;
    return g_presetEditRegion == CurrentMajorRegionForPreset();
}

WeatherPresetPackage BuildEditDraftPreview() {
    EnsureEditDraft();
    return g_editDraftPackage;
}

void ApplyDetectedRegionFromPackage(const WeatherPresetPackage& package) {
    const int regionId = CurrentMajorRegionForPreset();
    const WeatherPresetData data = EffectivePresetDataForRegion(package, regionId);
    ApplyPresetData(data);
    g_lastRuntimeAppliedData = data;
    g_lastRuntimeAppliedDataValid = true;
    g_lastAppliedRegion = regionId;
    g_regionBlendFrom = -1;
    g_regionBlendTo = -1;
    g_regionBlendLogBucket = -1;
    LogPresetDataSummary("runtime-apply-detected", regionId, data);
}

std::string g_rememberedPresetName;
bool g_rememberedPresetLoaded = false;
bool g_autoApplyRememberedTried = false;
bool g_autoApplyRememberedArmed = false;
ULONGLONG g_worldReadyStableStartTick = 0;
constexpr ULONGLONG kWorldReadyAutoApplyDelayMs = 1000;

void LoadRememberedPresetNameOnce() {
    if (g_rememberedPresetLoaded) return;
    g_rememberedPresetLoaded = true;

    char iniPath[MAX_PATH] = {};
    BuildIniPath(iniPath, sizeof(iniPath));
    char buf[MAX_PATH] = {};
    GetPrivateProfileStringA(
        kPresetConfigSection, kPresetConfigKeyLastPreset, "", buf, static_cast<DWORD>(sizeof(buf)), iniPath);
    g_rememberedPresetName = TrimCopy(buf);
    if (!g_rememberedPresetName.empty()) {
        Log("[preset] remembered: %s\n", g_rememberedPresetName.c_str());
    }
}

void PersistRememberedPresetName(const char* fileNameOrNull) {
    LoadRememberedPresetNameOnce();
    g_rememberedPresetName = fileNameOrNull ? TrimCopy(fileNameOrNull) : std::string();
    char iniPath[MAX_PATH] = {};
    BuildIniPath(iniPath, sizeof(iniPath));
    WritePrivateProfileStringA(
        kPresetConfigSection,
        kPresetConfigKeyLastPreset,
        g_rememberedPresetName.empty() ? "" : g_rememberedPresetName.c_str(),
        iniPath);
    Log("[preset] remembered preset saved as: %s\n",
        g_rememberedPresetName.empty() ? "<none>" : g_rememberedPresetName.c_str());
}

bool RememberedPresetMatches(const PresetListItem& item, const std::string& rememberedRaw) {
    if (rememberedRaw.empty()) return false;
    const std::string trimmed = TrimCopy(rememberedRaw);
    if (trimmed.empty()) return false;
    if (EqualsNoCase(item.fileName, trimmed)) return true;
    if (EqualsNoCase(item.displayName, trimmed)) return true;
    if (EqualsNoCase(GetPresetDisplayNameFromFileName(item.fileName), trimmed)) return true;
    return EqualsNoCase(item.fileName, EnsureIniExtension(trimmed));
}

float ActiveOverrideValue(const SliderOverride& overrideValue, float inactiveValue) {
    return overrideValue.active.load() ? overrideValue.value.load() : inactiveValue;
}

float OverrideValueIf(bool enabled, const SliderOverride& overrideValue, float inactiveValue) {
    return enabled ? overrideValue.value.load() : inactiveValue;
}

WeatherPresetColor ActiveColorOverrideValue(const ColorOverride& overrideValue, WeatherPresetColor inactiveValue) {
    if (!overrideValue.active.load()) {
        return inactiveValue;
    }
    return {
        overrideValue.r.load(),
        overrideValue.g.load(),
        overrideValue.b.load(),
        overrideValue.a.load(),
    };
}

void ApplyEnabledOverride(SliderOverride& overrideValue, bool enabled, float value, float lo, float hi) {
    if (enabled) {
        overrideValue.set(ClampPresetFloat(value, lo, hi));
    } else {
        overrideValue.clear();
    }
}

void ApplyEnabledColorOverride(ColorOverride& overrideValue, bool enabled, WeatherPresetColor value, bool includeAlpha) {
    if (enabled) {
        value = ClampPresetColor(value, includeAlpha);
        overrideValue.set(value.r, value.g, value.b, includeAlpha ? value.a : 1.0f);
    } else {
        overrideValue.clear();
    }
}

void ApplyPositiveOverride(SliderOverride& overrideValue, float value, float lo, float hi) {
    const float clamped = ClampPresetFloat(value, lo, hi);
    if (clamped > 0.0001f) {
        overrideValue.set(clamped);
    } else {
        overrideValue.clear();
    }
}

WeatherPresetColor RayleighColorFromBits(unsigned int bits) {
    return {
        static_cast<float>((bits >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((bits >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(bits & 0xFFu) / 255.0f,
        1.0f,
    };
}

WeatherPresetData CaptureCurrentPresetData() {
    const bool extendedSliderRange = g_extendedSliderRange.load();
    WeatherPresetData data{};
    data.forceClearSky = g_forceClear.load();
    data.noRain = g_noRain.load();
    data.rain = ActiveOverrideValue(g_oRain, 0.0f);
    data.thunder = ActiveOverrideValue(g_oThunder, 0.0f);
    data.noDust = g_noDust.load();
    data.dust = ActiveOverrideValue(g_oDust, 0.0f);
    data.noSnow = g_noSnow.load();
    data.snow = ActiveOverrideValue(g_oSnow, 0.0f);
    data.snowAccumBoundaryAEnabled = g_oSnowAccumBoundaryA.active.load();
    data.snowAccumBoundaryA = OverrideValueIf(data.snowAccumBoundaryAEnabled, g_oSnowAccumBoundaryA, -5.0f);
    data.snowAccumBoundaryBEnabled = g_oSnowAccumBoundaryB.active.load();
    data.snowAccumBoundaryB = OverrideValueIf(data.snowAccumBoundaryBEnabled, g_oSnowAccumBoundaryB, -20.0f);
    data.snowCoverageThresholdEnabled = g_oSnowCoverageThreshold.active.load();
    data.snowCoverageThreshold = OverrideValueIf(data.snowCoverageThresholdEnabled, g_oSnowCoverageThreshold, -20.0f);
    data.visualTimeOverride = g_timeCtrlActive.load() && g_timeFreeze.load();
    data.progressVisualTime = data.visualTimeOverride && g_timeProgressVisualTime.load();
    data.progressVisualTimeMatchGameTime = data.progressVisualTime && g_timeProgressMatchGameTime.load();
    data.progressVisualTimeIntervalMs = ClampPresetFloat(g_timeProgressCadenceMs.load(), 0.0f, 5000.0f);
    data.timeHour = NormalizeHour24(g_timeTargetHour.load());
    data.cloudAmountEnabled = g_oCloudAmount.active.load();
    data.cloudAmount = OverrideValueIf(data.cloudAmountEnabled, g_oCloudAmount, 1.0f);
    data.cloudHeightEnabled = g_oCloudSpdX.active.load();
    data.cloudHeight = OverrideValueIf(data.cloudHeightEnabled, g_oCloudSpdX, 1.0f);
    data.cloudDensityEnabled = g_oCloudSpdY.active.load();
    data.cloudDensity = OverrideValueIf(data.cloudDensityEnabled, g_oCloudSpdY, 1.0f);
    data.midCloudsEnabled = g_oHighClouds.active.load();
    data.midClouds = OverrideValueIf(data.midCloudsEnabled, g_oHighClouds, 1.0f);
    data.highCloudsEnabled = g_oAtmoAlpha.active.load();
    data.highClouds = OverrideValueIf(data.highCloudsEnabled, g_oAtmoAlpha, 1.0f);
    data.cloudAlphaEnabled = g_oCloudAlpha.active.load();
    data.cloudAlpha = OverrideValueIf(data.cloudAlphaEnabled, g_oCloudAlpha, g_windPackBase1E.load());
    data.cloudFadeRangeEnabled = g_oCloudFadeRange.active.load();
    data.cloudFadeRange = OverrideValueIf(data.cloudFadeRangeEnabled, g_oCloudFadeRange, g_windPackBase27.load());
    data.cloudDetailRatioEnabled = g_oCloudDetailRatio.active.load();
    data.cloudDetailRatio = OverrideValueIf(data.cloudDetailRatioEnabled, g_oCloudDetailRatio, g_windPackBase28.load());
    data.cloudPhaseFrontEnabled = g_oCloudPhaseFront.active.load();
    data.cloudPhaseFront = OverrideValueIf(data.cloudPhaseFrontEnabled, g_oCloudPhaseFront, g_windPackBase21.load());
    data.cloudScatteringCoefficientEnabled = g_oCloudScatteringCoefficient.active.load();
    data.cloudScatteringCoefficient = OverrideValueIf(data.cloudScatteringCoefficientEnabled, g_oCloudScatteringCoefficient, g_windPackBase20.load());
    data.cloudFlowEnabled = g_oCloudFlow.active.load();
    data.cloudFlow = OverrideValueIf(data.cloudFlowEnabled, g_oCloudFlow, g_windPackBase1F.load());
    data.cloudVisibleRangeEnabled = g_oCloudVisibleRange.active.load();
    data.cloudVisibleRange = OverrideValueIf(data.cloudVisibleRangeEnabled, g_oCloudVisibleRange, 1.0f);
    data.rayleighHeightEnabled = g_oRayleighHeight.active.load();
    data.rayleighHeight = OverrideValueIf(data.rayleighHeightEnabled, g_oRayleighHeight, g_windPackBase0E.load());
    data.ozoneRatioEnabled = g_oOzoneRatio.active.load();
    data.ozoneRatio = OverrideValueIf(data.ozoneRatioEnabled, g_oOzoneRatio, g_windPackBase14.load());
    data.rayleighScatteringColorEnabled = g_oRayleighScatteringColor.active.load();
    data.rayleighScatteringColor = ActiveColorOverrideValue(g_oRayleighScatteringColor, RayleighColorFromBits(g_windPackBase0FBits.load()));
    data.exp2CEnabled = g_oExpCloud2C.active.load();
    data.exp2C = OverrideValueIf(data.exp2CEnabled, g_oExpCloud2C, 1.0f);
    data.exp2DEnabled = g_oExpCloud2D.active.load();
    data.exp2D = OverrideValueIf(data.exp2DEnabled, g_oExpCloud2D, 1.0f);
    data.cloudVariationEnabled = g_oCloudVariation.active.load();
    data.cloudVariation = OverrideValueIf(data.cloudVariationEnabled, g_oCloudVariation, 1.0f);
    data.nightSkyRotationEnabled = g_oExpNightSkyRot.active.load();
    data.nightSkyRotation = OverrideValueIf(data.nightSkyRotationEnabled, g_oExpNightSkyRot,
        g_windPackBase0AValid.load() && g_windPackBase0BValid.load()
            ? ClampPresetFloat(g_windPackBase0A.load() + 90.0f - g_windPackBase0B.load(), -89.0f, 89.0f)
            : 0.0f);
    data.nightSkyYawEnabled = g_oNightSkyYaw.active.load();
    data.nightSkyYaw = OverrideValueIf(data.nightSkyYawEnabled, g_oNightSkyYaw, g_sceneBaseNightSkyYaw.load());
    data.sunSizeEnabled = g_oSunSize.active.load();
    data.sunSize = OverrideValueIf(data.sunSizeEnabled, g_oSunSize, g_atmoBaseSunSize.load());
    data.sunLightIntensityEnabled = g_oSunLightIntensity.active.load();
    data.sunLightIntensity = OverrideValueIf(data.sunLightIntensityEnabled, g_oSunLightIntensity, g_windPackBase00.load());
    data.sunYawEnabled = g_oSunDirX.active.load();
    data.sunYaw = OverrideValueIf(data.sunYawEnabled, g_oSunDirX, g_sceneBaseSunYaw.load());
    data.sunPitchEnabled = g_oSunDirY.active.load();
    data.sunPitch = OverrideValueIf(data.sunPitchEnabled, g_oSunDirY, g_sceneBaseSunPitch.load());
    data.moonSizeEnabled = g_oMoonSize.active.load();
    data.moonSize = OverrideValueIf(data.moonSizeEnabled, g_oMoonSize, g_atmoBaseMoonSize.load());
    data.moonLightIntensityEnabled = g_oMoonLightIntensity.active.load();
    data.moonLightIntensity = OverrideValueIf(data.moonLightIntensityEnabled, g_oMoonLightIntensity, g_windPackBase05.load());
    data.moonYawEnabled = g_oMoonDirX.active.load();
    data.moonYaw = OverrideValueIf(data.moonYawEnabled, g_oMoonDirX, g_sceneBaseMoonYaw.load());
    data.moonPitchEnabled = g_oMoonDirY.active.load();
    data.moonPitch = OverrideValueIf(data.moonPitchEnabled, g_oMoonDirY, g_sceneBaseMoonPitch.load());
    data.moonRollEnabled = g_oMoonRoll.active.load();
    data.moonRoll = OverrideValueIf(data.moonRollEnabled, g_oMoonRoll, 0.0f);
    const int moonTextureOption = MoonTextureSelectedOption();
    data.moonTextureEnabled = moonTextureOption > 0;
    data.moonTexture = data.moonTextureEnabled ? MoonTextureOptionName(moonTextureOption) : "";
    const int milkywayTextureOption = MilkywayTextureSelectedOption();
    data.milkywayTextureEnabled = milkywayTextureOption > 0;
    data.milkywayTexture = data.milkywayTextureEnabled ? MilkywayTextureOptionName(milkywayTextureOption) : "";
    data.fogEnabled = g_oFog.active.load();
    if (data.fogEnabled) {
        const float fogN = sqrtf(max(0.0f, g_oFog.value.load() / 100.0f));
        data.fogPercent = ClampPresetFogPercent(extendedSliderRange, fogN * 100.0f);
    } else {
        data.fogPercent = 0.0f;
    }
    data.nativeFogEnabled = g_oNativeFog.active.load();
    data.nativeFog = data.nativeFogEnabled ? ClampPresetNativeFog(extendedSliderRange, g_oNativeFog.value.load()) : 1.0f;
    data.volumeFogScatterColorEnabled = g_oVolumeFogScatterColor.active.load();
    data.volumeFogScatterColor = ActiveColorOverrideValue(g_oVolumeFogScatterColor, {
        g_windPackBase34.load(),
        g_windPackBase35.load(),
        g_windPackBase36.load(),
        g_windPackBase37.load(),
    });
    data.mieScatterColorEnabled = g_oMieScatterColor.active.load();
    data.mieScatterColor = ActiveColorOverrideValue(g_oMieScatterColor, {
        g_windPackBase38.load(),
        g_windPackBase39.load(),
        g_windPackBase3A.load(),
        g_windPackBase3B.load(),
    });
    data.mieScaleHeightEnabled = g_oMieScaleHeight.active.load();
    data.mieScaleHeight = OverrideValueIf(data.mieScaleHeightEnabled, g_oMieScaleHeight, g_windPackBase10.load());
    data.mieAerosolDensityEnabled = g_oMieAerosolDensity.active.load();
    data.mieAerosolDensity = OverrideValueIf(data.mieAerosolDensityEnabled, g_oMieAerosolDensity, g_windPackBase11.load());
    data.mieAerosolAbsorptionEnabled = g_oMieAerosolAbsorption.active.load();
    data.mieAerosolAbsorption = OverrideValueIf(data.mieAerosolAbsorptionEnabled, g_oMieAerosolAbsorption, g_windPackBase12.load());
    data.heightFogBaselineEnabled = g_oHeightFogBaseline.active.load();
    data.heightFogBaseline = OverrideValueIf(data.heightFogBaselineEnabled, g_oHeightFogBaseline, g_windPackBase18.load());
    data.heightFogFalloffEnabled = g_oHeightFogFalloff.active.load();
    data.heightFogFalloff = OverrideValueIf(data.heightFogFalloffEnabled, g_oHeightFogFalloff, g_windPackBase19.load());
    data.noFog = g_noFog.load();
    data.wind = ClampPresetWind(extendedSliderRange, g_windMul.load());
    data.noWind = g_noWind.load();
    data.puddleScaleEnabled = g_oCloudThk.active.load();
    data.puddleScale = data.puddleScaleEnabled ? ClampPresetPuddleScale(extendedSliderRange, g_oCloudThk.value.load()) : 0.0f;
    data.renodxAuroraRegionMaskEnabled = RenoDxBridgeIsAddonPresent();
    data.renodxAuroraGateEnabled = RenoDxBridgeIsAuroraGateEnabled();
    data.renodxAuroraRegionMask = RenoDxBridgeGetAuroraRegionMask();
    return data;
}

void ApplyPresetData(const WeatherPresetData& data) {
    const bool extendedSliderRange = g_extendedSliderRange.load();
    g_forceClear.store(data.forceClearSky);
    g_noRain.store(data.noRain);
    g_noDust.store(data.noDust);
    g_noSnow.store(data.noSnow);

    ApplyPositiveOverride(g_oRain, ClampPresetRain(extendedSliderRange, data.rain), 0.0f, 5.0f);
    ApplyPositiveOverride(g_oThunder, ClampPresetThunder(extendedSliderRange, data.thunder), 0.0f, 5.0f);
    ApplyPositiveOverride(g_oDust, ClampPresetDust(extendedSliderRange, data.dust), 0.0f, 10.0f);
    ApplyPositiveOverride(g_oSnow, ClampPresetSnow(extendedSliderRange, data.snow), 0.0f, 5.0f);
    ApplyEnabledOverride(g_oSnowAccumBoundaryA, data.snowAccumBoundaryAEnabled, ClampPresetSnowBoundary(data.snowAccumBoundaryA), -1000.0f, 1500.0f);
    ApplyEnabledOverride(g_oSnowAccumBoundaryB, data.snowAccumBoundaryBEnabled, ClampPresetSnowBoundary(data.snowAccumBoundaryB), -1000.0f, 1500.0f);
    ApplyEnabledOverride(g_oSnowCoverageThreshold, data.snowCoverageThresholdEnabled, ClampPresetSnowBoundary(data.snowCoverageThreshold), -1000.0f, 1500.0f);
    g_snowCoverageGlobalsDirty.store(true);

    const float nextTimeCadence = ClampPresetFloat(data.progressVisualTimeIntervalMs, 0.0f, 5000.0f);
    float nextTimeHour = NormalizeHour24(data.timeHour);
    const bool nextTimeCtrlActive = data.visualTimeOverride && g_timeLayoutReady.load();
    const bool nextTimeFreeze = nextTimeCtrlActive;
    const bool nextProgressVisualTime = nextTimeCtrlActive && data.progressVisualTime;
    const bool nextProgressMatchGameTime = nextProgressVisualTime && data.progressVisualTimeMatchGameTime;
    int nextMatchedHudMinute = -1;
    if (nextProgressMatchGameTime && g_timeUiClockSourceValid.load() && g_timeUiClockValid.load()) {
        const int hudHour = g_timeUiClockHour24.load();
        const int hudMinute = g_timeUiClockMinute.load();
        if (hudHour >= 0 && hudHour < 24 && hudMinute >= 0 && hudMinute < 60) {
            nextMatchedHudMinute = hudHour * 60 + hudMinute;
            nextTimeHour = NormalizeHour24(static_cast<float>(nextMatchedHudMinute) / 60.0f);
        }
    }
    const bool prevProgressVisualTime = g_timeProgressVisualTime.load();
    const bool prevProgressMatchGameTime = g_timeProgressMatchGameTime.load();
    const bool timeStateChanged =
        g_timeCtrlActive.load() != nextTimeCtrlActive ||
        g_timeFreeze.load() != nextTimeFreeze ||
        prevProgressVisualTime != nextProgressVisualTime ||
        prevProgressMatchGameTime != nextProgressMatchGameTime ||
        !HourNearlyEqual(g_timeTargetHour.load(), nextTimeHour) ||
        !FloatNearlyEqual(g_timeProgressCadenceMs.load(), nextTimeCadence);

    if (timeStateChanged) {
        g_timeProgressCadenceMs.store(nextTimeCadence);
        g_timeTargetHour.store(nextTimeHour);
        g_timeProgressVisualTime.store(nextProgressVisualTime);
        g_timeProgressMatchGameTime.store(nextProgressMatchGameTime);
        g_timeFreeze.store(nextTimeFreeze);
        g_timeCtrlActive.store(nextTimeCtrlActive);
        if (nextProgressMatchGameTime) {
            g_timeProgressLastTick.store(GetTickCount64());
            g_timeProgressMatchLastMinute.store(nextMatchedHudMinute);
            g_timeProgressMatchPendingMs.store(0);
        } else if (nextProgressVisualTime && (!prevProgressVisualTime || nextProgressMatchGameTime != prevProgressMatchGameTime)) {
            g_timeProgressLastTick.store(GetTickCount64());
            g_timeProgressMatchLastMinute.store(-1);
            g_timeProgressMatchPendingMs.store(0);
        } else if (!nextProgressVisualTime) {
            g_timeProgressLastTick.store(0);
            g_timeProgressMatchLastMinute.store(-1);
            g_timeProgressMatchPendingMs.store(0);
        }
        g_timeApplyRequest.store(true);
    }
    if (timeStateChanged && data.visualTimeOverride && !g_timeLayoutReady.load()) {
        Log("[W] preset visual time skipped: time layout unresolved\n");
    }

    ApplyEnabledOverride(g_oCloudAmount, data.cloudAmountEnabled, ClampPresetCloudAmount(extendedSliderRange, data.cloudAmount), 0.0f, 50.0f);
    ApplyEnabledOverride(g_oCloudSpdX, data.cloudHeightEnabled, ClampPresetCloudHeight(extendedSliderRange, data.cloudHeight), -50.0f, 50.0f);
    ApplyEnabledOverride(g_oCloudSpdY, data.cloudDensityEnabled, ClampPresetCloudDensity(extendedSliderRange, data.cloudDensity), 0.0f, 50.0f);
    ApplyEnabledOverride(g_oHighClouds, data.midCloudsEnabled, ClampPresetCloudWide(extendedSliderRange, data.midClouds), 0.0f, 50.0f);
    ApplyEnabledOverride(g_oAtmoAlpha, data.highCloudsEnabled, ClampPresetCloudWide(extendedSliderRange, data.highClouds), 0.0f, 50.0f);
    ApplyEnabledOverride(g_oCloudAlpha, data.cloudAlphaEnabled, ClampPresetCloudAlpha(extendedSliderRange, data.cloudAlpha), 0.0f, 100.0f);
    ApplyEnabledOverride(g_oCloudFadeRange, data.cloudFadeRangeEnabled, ClampPresetCloudFadeRange(extendedSliderRange, data.cloudFadeRange), 0.0f, 200000.0f);
    ApplyEnabledOverride(g_oCloudDetailRatio, data.cloudDetailRatioEnabled, ClampPresetCloudDetailRatio(data.cloudDetailRatio), 0.0f, 1.5f);
    ApplyEnabledOverride(g_oCloudPhaseFront, data.cloudPhaseFrontEnabled, ClampPresetCloudPhaseFront(extendedSliderRange, data.cloudPhaseFront), -1.0f, 1.0f);
    ApplyEnabledOverride(g_oCloudScatteringCoefficient, data.cloudScatteringCoefficientEnabled, ClampPresetCloudScatteringCoefficient(extendedSliderRange, data.cloudScatteringCoefficient), kCloudScatteringCoefficientMin, 100.0f);
    ApplyEnabledOverride(g_oCloudFlow, data.cloudFlowEnabled, ClampPresetCloudFlow(extendedSliderRange, data.cloudFlow), 0.0f, 50.0f);
    ApplyEnabledOverride(g_oCloudVisibleRange, data.cloudVisibleRangeEnabled, ClampPresetCloudVisibleRange(data.cloudVisibleRange), 0.0f, 10.0f);
    ApplyEnabledOverride(g_oRayleighHeight, data.rayleighHeightEnabled, ClampPresetRayleighHeight(extendedSliderRange, data.rayleighHeight), 1.0f, 200000.0f);
    ApplyEnabledOverride(g_oOzoneRatio, data.ozoneRatioEnabled, ClampPresetOzoneRatio(extendedSliderRange, data.ozoneRatio), 0.0f, 100.0f);
    ApplyEnabledColorOverride(g_oRayleighScatteringColor, data.rayleighScatteringColorEnabled, data.rayleighScatteringColor, false);
    ApplyEnabledOverride(g_oExpCloud2C, data.exp2CEnabled, ClampPresetCloudWide(extendedSliderRange, data.exp2C), 0.0f, 50.0f);
    ApplyEnabledOverride(g_oExpCloud2D, data.exp2DEnabled, ClampPresetCloudWide(extendedSliderRange, data.exp2D), 0.0f, 50.0f);
    ApplyEnabledOverride(g_oCloudVariation, data.cloudVariationEnabled, ClampPresetCloudWide(extendedSliderRange, data.cloudVariation), 0.0f, 50.0f);
    ApplyEnabledOverride(g_oExpNightSkyRot, data.nightSkyRotationEnabled, ClampPresetPitch(extendedSliderRange, data.nightSkyRotation), -180.0f, 180.0f);
    ApplyEnabledOverride(g_oNightSkyYaw, data.nightSkyYawEnabled, ClampPresetYaw(extendedSliderRange, data.nightSkyYaw), -360.0f, 360.0f);
    ApplyEnabledOverride(g_oSunSize, data.sunSizeEnabled, ClampPresetSunSize(extendedSliderRange, data.sunSize), 0.001f, 100.0f);
    ApplyEnabledOverride(g_oSunLightIntensity, data.sunLightIntensityEnabled, ClampPresetLightIntensity(extendedSliderRange, data.sunLightIntensity), 0.0f, 100.0f);
    ApplyEnabledOverride(g_oSunDirX, data.sunYawEnabled, ClampPresetYaw(extendedSliderRange, data.sunYaw), -360.0f, 360.0f);
    ApplyEnabledOverride(g_oSunDirY, data.sunPitchEnabled, ClampPresetPitch(extendedSliderRange, data.sunPitch), -180.0f, 180.0f);
    ApplyEnabledOverride(g_oMoonSize, data.moonSizeEnabled, ClampPresetMoonSize(extendedSliderRange, data.moonSize), 0.001f, 100.0f);
    ApplyEnabledOverride(g_oMoonLightIntensity, data.moonLightIntensityEnabled, ClampPresetLightIntensity(extendedSliderRange, data.moonLightIntensity), 0.0f, 100.0f);
    ApplyEnabledOverride(g_oMoonDirX, data.moonYawEnabled, ClampPresetYaw(extendedSliderRange, data.moonYaw), -360.0f, 360.0f);
    ApplyEnabledOverride(g_oMoonDirY, data.moonPitchEnabled, ClampPresetPitch(extendedSliderRange, data.moonPitch), -180.0f, 180.0f);
    ApplyEnabledOverride(g_oMoonRoll, data.moonRollEnabled, ClampPresetYaw(extendedSliderRange, data.moonRoll), -360.0f, 360.0f);
    MoonTextureSelectByName(data.moonTextureEnabled ? data.moonTexture.c_str() : nullptr);
    MilkywayTextureSelectByName(data.milkywayTextureEnabled ? data.milkywayTexture.c_str() : nullptr);

    const float fogPct = ClampPresetFogPercent(extendedSliderRange, data.fogPercent);
    if (data.fogEnabled) {
        const float t = fogPct * 0.01f;
        const float fogBoost = t * t * 100.0f;
        g_oFog.set(fogBoost);
    } else g_oFog.clear();

    const float nativeFog = ClampPresetNativeFog(extendedSliderRange, data.nativeFog);
    if (data.nativeFogEnabled && fabsf(nativeFog - 1.0f) > 0.001f) g_oNativeFog.set(nativeFog);
    else g_oNativeFog.clear();

    ApplyEnabledColorOverride(g_oVolumeFogScatterColor, data.volumeFogScatterColorEnabled, data.volumeFogScatterColor, true);
    ApplyEnabledColorOverride(g_oMieScatterColor, data.mieScatterColorEnabled, data.mieScatterColor, true);
    ApplyEnabledOverride(g_oMieScaleHeight, data.mieScaleHeightEnabled, ClampPresetMieScaleHeight(extendedSliderRange, data.mieScaleHeight), 1.0f, 200000.0f);
    ApplyEnabledOverride(g_oMieAerosolDensity, data.mieAerosolDensityEnabled, ClampPresetMieDensity(extendedSliderRange, data.mieAerosolDensity), 0.0f, 100.0f);
    ApplyEnabledOverride(g_oMieAerosolAbsorption, data.mieAerosolAbsorptionEnabled, ClampPresetMieAbsorption(extendedSliderRange, data.mieAerosolAbsorption), 0.0f, 100.0f);
    ApplyEnabledOverride(g_oHeightFogBaseline, data.heightFogBaselineEnabled, ClampPresetHeightFogBaseline(extendedSliderRange, data.heightFogBaseline), -50000.0f, 50000.0f);
    ApplyEnabledOverride(g_oHeightFogFalloff, data.heightFogFalloffEnabled, ClampPresetHeightFogFalloff(extendedSliderRange, data.heightFogFalloff), 0.0f, 100.0f);

    g_noFog.store(data.noFog);
    const float wind = ClampPresetWind(extendedSliderRange, data.wind);
    g_windMul.store(wind);
    g_noWind.store(data.noWind);
    ApplyEnabledOverride(g_oCloudThk, data.puddleScaleEnabled, ClampPresetPuddleScale(extendedSliderRange, data.puddleScale), 0.0f, 5.0f);
    if (data.renodxAuroraRegionMaskEnabled) {
        RenoDxBridgeApplyPresetAuroraSettings(data.renodxAuroraGateEnabled, data.renodxAuroraRegionMask);
    }
}

bool SanitizeMissingMoonTexture(WeatherPresetData& data, const char* scopeLabel) {
    if (!data.moonTextureEnabled || data.moonTexture.empty()) {
        return false;
    }

    if (MoonTextureFindOptionByName(data.moonTexture.c_str()) >= 0) {
        return false;
    }

    Log("[preset] missing moon texture in %s: %s -> Native\n", scopeLabel, data.moonTexture.c_str());
    data.moonTextureEnabled = false;
    data.moonTexture.clear();
    return true;
}

bool SanitizeMissingMoonTexturesInPackage(WeatherPresetPackage& package) {
    MoonTextureRefreshList();
    MilkywayTextureRefreshList();

    bool changed = SanitizeMissingMoonTexture(package.global, "Global");
    if (package.global.milkywayTextureEnabled && !package.global.milkywayTexture.empty() &&
        MilkywayTextureFindOptionByName(package.global.milkywayTexture.c_str()) < 0) {
        Log("[preset] missing milkyway texture in Global: %s -> Native\n", package.global.milkywayTexture.c_str());
        package.global.milkywayTextureEnabled = false;
        package.global.milkywayTexture.clear();
        changed = true;
    }
    for (int regionId = 1; regionId < kPresetRegionCount; ++regionId) {
        if (package.regionEnabled[regionId] && package.regionMask[regionId].moonTexture) {
            changed = SanitizeMissingMoonTexture(package.region[regionId], RegionDisplayName(regionId)) || changed;
        }
        if (package.regionEnabled[regionId] && package.regionMask[regionId].milkywayTexture &&
            package.region[regionId].milkywayTextureEnabled && !package.region[regionId].milkywayTexture.empty() &&
            MilkywayTextureFindOptionByName(package.region[regionId].milkywayTexture.c_str()) < 0) {
            Log("[preset] missing milkyway texture in %s: %s -> Native\n", RegionDisplayName(regionId), package.region[regionId].milkywayTexture.c_str());
            package.region[regionId].milkywayTextureEnabled = false;
            package.region[regionId].milkywayTexture.clear();
            changed = true;
        }
    }
    return changed;
}

void SanitizeAndPersistPresetPackageIfNeeded(const PresetListItem& item, WeatherPresetPackage& package) {
    if (!SanitizeMissingMoonTexturesInPackage(package)) {
        return;
    }

    if (WritePresetPackageInternal(item.fullPath.c_str(), CurrentPresetFormatOptions(), package)) {
        Log("[preset] repaired missing moon texture entries in %s\n", item.fileName.c_str());
    } else {
        Log("[W] failed to repair missing moon texture entries in %s\n", item.fileName.c_str());
    }
}

bool IsValidUserPresetName(const std::string& rawName, std::string& outFileName) {
    std::string trimmed = TrimCopy(rawName);
    if (trimmed.empty()) return false;
    static constexpr const char* kInvalidChars = "\\/:*?\"<>|";
    if (trimmed.find_first_of(kInvalidChars) != std::string::npos) return false;
    if (!EndsWithIni(trimmed)) trimmed += ".ini";
    outFileName = trimmed;
    return true;
}

void RefreshPresetListInternal() {
    const std::string oldSelection = HasSelectedPresetIndexInternal()
        ? g_presetItems[g_selectedPresetIndex].fileName
        : std::string();

    std::vector<PresetListItem> foundItems;
    const auto addFromDirectory = [&foundItems](const std::string& dir, const char* fileNamePrefix, const char* displayPrefix) {
    WIN32_FIND_DATAA fd{};
    HANDLE find = FindFirstFileA(JoinPath(dir, "*.ini").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            const std::string fileName = fd.cFileName;
            if (!EndsWithIni(fileName)) continue;
            const std::string fullPath = JoinPath(dir, fileName);
            if (!IsValidPresetFile(fullPath.c_str())) continue;
            foundItems.push_back({
                std::string(fileNamePrefix ? fileNamePrefix : "") + fileName,
                std::string(displayPrefix ? displayPrefix : "") + GetPresetDisplayNameFromFileName(fileName),
                fullPath });
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    };

    addFromDirectory(GetPresetDirectory(), "", "");
    addFromDirectory(GetCommunityPresetDirectory(), "CrimsonWeather\\community\\preset\\", "");

    std::sort(foundItems.begin(), foundItems.end(), [](const PresetListItem& a, const PresetListItem& b) {
        return _stricmp(a.displayName.c_str(), b.displayName.c_str()) < 0;
    });

    g_presetItems.swap(foundItems);
    g_selectedPresetIndex = -1;
    if (!oldSelection.empty()) {
        for (int i = 0; i < static_cast<int>(g_presetItems.size()); ++i) {
            if (EqualsNoCase(g_presetItems[i].fileName, oldSelection)) {
                g_selectedPresetIndex = i;
                break;
            }
        }
    }
    if (g_selectedPresetIndex < 0 && !g_rememberedPresetName.empty()) {
        for (int i = 0; i < static_cast<int>(g_presetItems.size()); ++i) {
            if (RememberedPresetMatches(g_presetItems[i], g_rememberedPresetName)) {
                g_selectedPresetIndex = i;
                break;
            }
        }
    }
}

void RefreshSelectedPresetBaselineFromDisk() {
    if (!HasSelectedPresetIndexInternal()) {
        ClearSelectedPresetBaseline();
        return;
    }
    WeatherPresetPackage package{};
    if (!LoadPresetPackageInternal(g_presetItems[g_selectedPresetIndex].fullPath.c_str(), CurrentPresetFormatOptions(), package)) {
        ClearSelectedPresetBaseline();
        return;
    }
    SanitizeAndPersistPresetPackageIfNeeded(g_presetItems[g_selectedPresetIndex], package);
    SetSelectedPresetBaseline(package);
}

int FindPresetIndexByFileName(const std::string& rawFileName) {
    const std::string fileName = EnsureIniExtension(TrimCopy(rawFileName));
    if (fileName.empty()) {
        return -1;
    }
    for (int i = 0; i < static_cast<int>(g_presetItems.size()); ++i) {
        if (EqualsNoCase(g_presetItems[i].fileName, fileName)) {
            return i;
        }
    }
    return -1;
}


bool ScheduleLoadPresetForRuntime(int index, WeatherPresetPackage& package) {
    if (index < 0 || index >= static_cast<int>(g_presetItems.size())) {
        return false;
    }
    if (!LoadPresetPackageInternal(g_presetItems[index].fullPath.c_str(), CurrentPresetFormatOptions(), package)) {
        return false;
    }
    SanitizeAndPersistPresetPackageIfNeeded(g_presetItems[index], package);
    return true;
}

void ScheduleEnsureInitialized() {
    if (g_presetsInitialized) {
        return;
    }
    LoadRememberedPresetNameOnce();
    RefreshPresetListInternal();
    RefreshSelectedPresetBaselineFromDisk();
    g_presetsInitialized = true;
    Log("[preset-schedule] initialized preset list for runtime schedule (%d preset(s))\n",
        static_cast<int>(g_presetItems.size()));
}

void ScheduleApplyData(const WeatherPresetData& data, int regionId) {
    ApplyPresetData(data);
    g_lastRuntimeAppliedData = data;
    g_lastRuntimeAppliedDataValid = true;
    g_lastAppliedRegion = regionId;
    g_regionBlendFrom = -1;
    g_regionBlendTo = -1;
    g_regionBlendLogBucket = -1;
}

bool ScheduleEditDraftValid() {
    return g_editDraftValid;
}

int ScheduleLastAppliedRegion() {
    return g_lastAppliedRegion;
}
void MarkAutoSavePending() {
    if (!g_cfg.autoSaved) {
        g_autoSavePending = false;
        return;
    }
    if (!HasSelectedPresetIndexInternal()) {
        return;
    }
    g_autoSavePending = true;
    g_autoSaveLastEditTick = GetTickCount64();
}

bool SaveSelectedDraftInternal(const char* statusPrefix, const char* logVerb, bool applyAfterSave) {
    if (!Preset_HasSelection()) return false;
    WeatherPresetPackage package = BuildEditDraftPreview();
    const PresetListItem item = g_presetItems[g_selectedPresetIndex];
    if (!WritePresetPackageInternal(item.fullPath.c_str(), CurrentPresetFormatOptions(), package)) {
        GUI_SetStatus("Preset save failed");
        Log("[preset] failed to save %s\n", item.fileName.c_str());
        return false;
    }
    SetSelectedPresetBaseline(package);
    PersistRememberedPresetName(item.fileName.c_str());
    if (applyAfterSave) {
        ApplyDetectedRegionFromPackage(package);
    }
    GUI_SetStatus((std::string(statusPrefix ? statusPrefix : "Preset saved: ") + item.displayName).c_str());
    Log("[preset] %s %s\n", logVerb ? logVerb : "saved", item.fileName.c_str());
    LogPresetPackageSummary(logVerb && strcmp(logVerb, "autosaved") == 0 ? "autosaved-package" : "saved-package", package);
    return true;
}
} // namespace

namespace preset_internal {

const PresetScheduleHost& GetPresetScheduleHost() {
    static const PresetScheduleHost host = {
        &FindPresetIndexByFileName,
        &HasSelectedPresetIndexInternal,
        []() { return g_selectedPresetIndex; },
        []() { return static_cast<int>(g_presetItems.size()); },
        [](int index) -> const PresetListItem& { return g_presetItems[index]; },
        &ScheduleEnsureInitialized,
        &ScheduleLoadPresetForRuntime,
        &SelectPresetIndexInternal,
        &CaptureCurrentPresetData,
        &CurrentMajorRegionForPreset,
        &ScheduleApplyData,
        &ScheduleEditDraftValid,
        &ScheduleLastAppliedRegion
    };
    return host;
}

} // namespace preset_internal
void Preset_EnsureInitialized() {
    if (g_presetsInitialized) return;
    LoadRememberedPresetNameOnce();
    EnsureTimeScheduleLoaded();
    Preset_Refresh();
}

void Preset_Refresh() {
    RefreshPresetListInternal();
    RefreshSelectedPresetBaselineFromDisk();
    g_presetsInitialized = true;
    Log("[preset] refresh -> %d valid preset(s)\n", static_cast<int>(g_presetItems.size()));
}

int Preset_GetCount() {
    return static_cast<int>(g_presetItems.size());
}

const char* Preset_GetDisplayName(int index) {
    if (index < 0 || index >= static_cast<int>(g_presetItems.size())) return "";
    return g_presetItems[index].displayName.c_str();
}

const char* Preset_GetFileName(int index) {
    if (index < 0 || index >= static_cast<int>(g_presetItems.size())) return "";
    return g_presetItems[index].fileName.c_str();
}

bool Preset_IsCommunityPreset(int index) {
    if (index < 0 || index >= static_cast<int>(g_presetItems.size())) return false;
    constexpr const char* kCommunityPresetPrefix = "CrimsonWeather\\community\\preset\\";
    return _strnicmp(
        g_presetItems[index].fileName.c_str(),
        kCommunityPresetPrefix,
        strlen(kCommunityPresetPrefix)) == 0;
}

bool Preset_GetCommunityInstallInfo(int index, CommunityPresetInstallInfo& outInfo) {
    outInfo = CommunityPresetInstallInfo{};
    if (!Preset_IsCommunityPreset(index)) return false;
    outInfo.displayName = g_presetItems[index].displayName;
    outInfo.fullPath = g_presetItems[index].fullPath;
    ReadCommunityMetadataFromPresetFile(g_presetItems[index].fullPath.c_str(), outInfo);
    outInfo.valid = true;
    return true;
}

int Preset_GetSelectedIndex() {
    return g_selectedPresetIndex;
}

bool Preset_HasSelection() {
    return HasSelectedPresetIndexInternal();
}

const char* Preset_GetSelectedDisplayName() {
    return Preset_HasSelection() ? g_presetItems[g_selectedPresetIndex].displayName.c_str() : kNewPresetDisplayName;
}

const char* Preset_GetRegionDisplayName(int regionId) {
    return RegionDisplayName(regionId);
}

int Preset_GetEditRegion() {
    return g_presetEditRegion;
}

void Preset_SetEditRegion(int regionId) {
    if (!IsPresetRegionId(regionId)) regionId = kPresetRegionGlobal;
    if (regionId == g_presetEditRegion) return;
    g_presetEditRegion = regionId;
    Log("[preset] edit scope selected: %s (player region %s)\n",
        RegionDisplayName(g_presetEditRegion),
        RegionDisplayName(CurrentMajorRegionForPreset()));
    EnsureEditDraft();
    if (g_presetEditRegion > kPresetRegionGlobal) {
        LogRegionMaskSummary("selected-region-mask", g_presetEditRegion, g_editDraftPackage);
    }
    LogPresetDataSummary(
        "selected-region-effective",
        g_presetEditRegion,
        EffectivePresetDataForRegion(g_editDraftPackage, g_presetEditRegion));
}

bool Preset_SelectedHasRegion(int regionId) {
    if (regionId <= kPresetRegionGlobal || regionId >= kPresetRegionCount) {
        return false;
    }
    return BuildEditDraftPreview().regionEnabled[regionId];
}

bool Preset_IsEditingDetachedRegion() {
    return true;
}

WeatherPresetData Preset_GetEditRegionData() {
    EnsureEditDraft();
    return EffectivePresetDataForRegion(g_editDraftPackage, g_presetEditRegion);
}

WeatherPresetSourceMask Preset_GetEditRegionOverrideMask() {
    EnsureEditDraft();
    if (g_presetEditRegion <= kPresetRegionGlobal || g_presetEditRegion >= kPresetRegionCount) {
        return WeatherPresetSourceMask{};
    }
    if (!g_editDraftPackage.regionEnabled[g_presetEditRegion]) {
        return WeatherPresetSourceMask{};
    }
    return ToSourceMask(g_editDraftPackage.regionMask[g_presetEditRegion]);
}

void Preset_SetEditRegionData(const WeatherPresetData& data) {
    EnsureEditDraft();
    if (!HasSelectedPresetIndexInternal()) {
        g_newPresetDraftActive = true;
    }
    TimeSchedulePinCurrentEntryForUserEdit();
    const bool affectsRuntime = EditRegionAffectsCurrentRuntime();
    SaveEditedRegionToPackage(g_editDraftPackage, g_presetEditRegion, data);
    if (affectsRuntime) {
        ApplyDetectedRegionFromPackage(g_editDraftPackage);
    }
    MarkAutoSavePending();
}

void Preset_SetEditRegionDataWithOverrides(const WeatherPresetData& data, const WeatherPresetSourceMask& mask) {
    EnsureEditDraft();
    if (!HasSelectedPresetIndexInternal()) {
        g_newPresetDraftActive = true;
    }
    TimeSchedulePinCurrentEntryForUserEdit();
    const bool affectsRuntime = EditRegionAffectsCurrentRuntime();
    SaveEditedRegionToPackageWithMask(g_editDraftPackage, g_presetEditRegion, data, FromSourceMask(mask));
    if (affectsRuntime) {
        ApplyDetectedRegionFromPackage(g_editDraftPackage);
    }
    MarkAutoSavePending();
}

void Preset_SetRenoDxAuroraSettings(bool enabled, uint32_t mask) {
    EnsureEditDraft();
    if (!HasSelectedPresetIndexInternal()) {
        g_newPresetDraftActive = true;
    }
    TimeSchedulePinCurrentEntryForUserEdit();
    g_editDraftPackage.global.renodxAuroraRegionMaskEnabled = RenoDxBridgeIsAddonPresent();
    g_editDraftPackage.global.renodxAuroraGateEnabled = enabled;
    g_editDraftPackage.global.renodxAuroraRegionMask = mask & 126u;
    Log("[preset] edit-global RenoDX aurora enabled=%u mask=0x%02X\n",
        enabled ? 1u : 0u,
        g_editDraftPackage.global.renodxAuroraRegionMask);
    MarkAutoSavePending();
}

void Preset_ResetEditRegion() {
    EnsureEditDraft();
    if (!HasSelectedPresetIndexInternal()) {
        g_newPresetDraftActive = true;
    }
    TimeSchedulePinCurrentEntryForUserEdit();
    if (g_presetEditRegion > kPresetRegionGlobal && g_presetEditRegion < kPresetRegionCount) {
        ResetRegionToDefaultsInPackage(g_editDraftPackage, g_presetEditRegion);
        const bool affectsRuntime = EditRegionAffectsCurrentRuntime();
        if (!affectsRuntime) {
            Log("[preset] reset deferred until player enters %s\n", RegionDisplayName(g_presetEditRegion));
        }
        if (affectsRuntime) {
            ApplyDetectedRegionFromPackage(g_editDraftPackage);
        }
        MarkAutoSavePending();
        GUI_SetStatus(("Region overrides cleared: " + std::string(RegionDisplayName(g_presetEditRegion))).c_str());
        return;
    }

    SaveEditedRegionToPackage(g_editDraftPackage, kPresetRegionGlobal, WeatherPresetData{});
    ApplyDetectedRegionFromPackage(g_editDraftPackage);
    MarkAutoSavePending();
    GUI_SetStatus("Global reset to defaults");
}

void Preset_SelectNew() {
    TimeScheduleDisableForManualSelection();
    g_selectedPresetIndex = -1;
    ClearSelectedPresetBaseline();
    g_newPresetDraftActive = true;
    PersistRememberedPresetName(nullptr);
    Log("[preset] selected [New Preset]; draft package active\n");
    LogPresetPackageSummary("new-preset-draft", g_editDraftPackage);
}

bool Preset_HasUnsavedChanges() {
    if (!Preset_HasSelection()) return true;
    if (!g_selectedPresetBaselineValid) return true;
    return !PresetPackageEquals(BuildEditDraftPreview(), g_selectedPresetBaseline);
}

bool Preset_CanSaveCurrent() {
    if (!Preset_HasSelection()) return true;
    return Preset_HasUnsavedChanges();
}

void Preset_AutoSaveTick(bool uiEditActive) {
    if (!g_cfg.autoSaved) {
        g_autoSavePending = false;
        return;
    }
    if (!g_autoSavePending || uiEditActive || !HasSelectedPresetIndexInternal()) {
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_autoSaveLastEditTick < kAutoSaveDebounceMs) {
        return;
    }
    if (!Preset_HasUnsavedChanges()) {
        g_autoSavePending = false;
        return;
    }
    SaveSelectedDraftInternal("Preset autosaved: ", "autosaved", false);
}

WeatherPresetStatusSnapshot Preset_GetStatusSnapshot() {
    WeatherPresetStatusSnapshot snapshot{};
    snapshot.playerRegion = CurrentMajorRegionForPreset();
    snapshot.editRegion = g_presetEditRegion;
    snapshot.blendFromRegion = g_regionBlendFrom;
    snapshot.blendToRegion = g_regionBlendTo;

    const float remaining = g_regionTransitionSeconds.load();
    snapshot.blendProgress = 1.0f - ClampPresetFloat(remaining / kRegionTransitionDurationSeconds, 0.0f, 1.0f);

    const bool hasSelectedRuntimePackage = HasSelectedPresetIndexInternal() && g_selectedPresetBaselineValid;
    const bool hasNewRuntimePackage = !HasSelectedPresetIndexInternal() && g_newPresetDraftActive;
    snapshot.hasPresetPackage = hasSelectedRuntimePackage || hasNewRuntimePackage || g_editDraftValid;

    if (!snapshot.hasPresetPackage) {
        snapshot.effective = CaptureCurrentPresetData();
        return snapshot;
    }

    EnsureEditDraft();
    const WeatherPresetPackage& package = g_editDraftPackage;
    snapshot.effective = g_lastRuntimeAppliedDataValid
        ? g_lastRuntimeAppliedData
        : EffectivePresetDataForRegion(package, snapshot.playerRegion);

    if (snapshot.playerRegion > kPresetRegionGlobal &&
        snapshot.playerRegion < kPresetRegionCount &&
        package.regionEnabled[snapshot.playerRegion]) {
        snapshot.regionSource = ToSourceMask(package.regionMask[snapshot.playerRegion]);
    }

    return snapshot;
}

bool Preset_CurrentSlidersMatchAppliedRegion() {
    if (!g_selectedPresetBaselineValid || g_lastAppliedRegion < 0) return true;
    const WeatherPresetPackage& runtimePackage = g_editDraftValid ? g_editDraftPackage : g_selectedPresetBaseline;
    const WeatherPresetData current = CaptureCurrentPresetData();
    if (g_lastRuntimeAppliedDataValid) {
        return PresetDataEquals(current, g_lastRuntimeAppliedData);
    }
    return PresetDataEquals(current, EffectivePresetDataForRegion(runtimePackage, g_lastAppliedRegion));
}

bool SelectPresetIndexInternal(int index, bool applyImmediately, const char* statusPrefix, const char* toastPrefix, const char* logVerb) {
    if (index < 0 || index >= static_cast<int>(g_presetItems.size())) return false;
    WeatherPresetPackage package{};
    if (!LoadPresetPackageInternal(g_presetItems[index].fullPath.c_str(), CurrentPresetFormatOptions(), package)) {
        GUI_SetStatus("Preset load failed");
        Log("[preset] failed to load %s\n", g_presetItems[index].fileName.c_str());
        return false;
    }
    SanitizeAndPersistPresetPackageIfNeeded(g_presetItems[index], package);
    if (applyImmediately) {
        ApplyDetectedRegionFromPackage(package);
    } else {
        g_lastAppliedRegion = -1;
        g_lastRuntimeAppliedData = WeatherPresetData{};
        g_lastRuntimeAppliedDataValid = false;
        g_regionBlendFrom = -1;
        g_regionBlendTo = -1;
        g_regionBlendLogBucket = -1;
    }
    g_selectedPresetIndex = index;
    SetSelectedPresetBaseline(package);
    PersistRememberedPresetName(g_presetItems[index].fileName.c_str());
    GUI_SetStatus((std::string(statusPrefix ? statusPrefix : "Preset loaded: ") + g_presetItems[index].displayName).c_str());
    if (toastPrefix && toastPrefix[0]) {
        ShowNativeToast((std::string(toastPrefix) + g_presetItems[index].displayName).c_str());
    }
    Log("[preset] %s %s\n", logVerb ? logVerb : "loaded", g_presetItems[index].fileName.c_str());
    LogPresetPackageSummary("loaded-package", package);
    return true;
}

bool Preset_SelectIndex(int index) {
    TimeScheduleDisableForManualSelection();
    return SelectPresetIndexInternal(index, true, "Preset loaded: ", "ACTIVATED PRESET: ", "loaded");
}

bool Preset_SaveSelected() {
    return SaveSelectedDraftInternal("Preset saved: ", "saved", true);
}

bool Preset_SaveAs(const char* fileName) {
    std::string normalizedName;
    if (!IsValidUserPresetName(fileName ? fileName : "", normalizedName)) {
        GUI_SetStatus("Invalid preset name");
        return false;
    }

    WeatherPresetPackage package = BuildEditDraftPreview();
    const std::string fullPath = JoinPath(GetPresetDirectory(), normalizedName);
    if (!WritePresetPackageInternal(fullPath.c_str(), CurrentPresetFormatOptions(), package)) {
        GUI_SetStatus("Preset save failed");
        Log("[preset] failed to save %s\n", normalizedName.c_str());
        return false;
    }

    RefreshPresetListInternal();
    ClearSelectedPresetBaseline();
    for (int i = 0; i < static_cast<int>(g_presetItems.size()); ++i) {
        if (EqualsNoCase(g_presetItems[i].fileName, normalizedName)) {
            g_selectedPresetIndex = i;
            SetSelectedPresetBaseline(package);
            break;
        }
    }
    PersistRememberedPresetName(normalizedName.c_str());
    ApplyDetectedRegionFromPackage(package);

    GUI_SetStatus(("Preset saved: " + GetPresetDisplayNameFromFileName(normalizedName)).c_str());
    Log("[preset] saved %s\n", normalizedName.c_str());
    LogPresetPackageSummary("saved-as-package", package);
    return true;
}

bool Preset_ExportCurrentCanonical(std::string& outIni, std::string& outError) {
    outIni.clear();
    outError.clear();
    EnsureEditDraft();
    WeatherPresetPackage package = BuildEditDraftPreview();
    outIni = SerializePresetPackage(package, CurrentPresetFormatOptions());
    if (outIni.empty()) {
        outError = "Current preset could not be serialized";
        return false;
    }
    return true;
}

bool Preset_ExportPresetCanonicalByIndex(int index, std::string& outIni, std::string& outError) {
    outIni.clear();
    outError.clear();
    Preset_EnsureInitialized();
    if (index < 0 || index >= static_cast<int>(g_presetItems.size())) {
        outError = "Selected preset is not available";
        return false;
    }
    WeatherPresetPackage package{};
    if (!LoadPresetPackageInternal(g_presetItems[index].fullPath.c_str(), CurrentPresetFormatOptions(), package)) {
        outError = "Selected preset failed validation";
        return false;
    }
    outIni = SerializePresetPackage(package, CurrentPresetFormatOptions());
    if (outIni.empty()) {
        outError = "Selected preset could not be serialized";
        return false;
    }
    return true;
}

std::string SanitizeCommunityPresetFileName(const char* title, const char* author) {
    std::string rawTitle = TrimCopy(title ? title : "");
    if (rawTitle.empty()) {
        rawTitle = "Community Preset";
    }
    std::string rawAuthor = TrimCopy(author ? author : "");
    if (rawAuthor.empty()) {
        rawAuthor = "Anonymous";
    }
    std::string raw = rawTitle + " by " + rawAuthor;
    static constexpr const char* kInvalidChars = "\\/:*?\"<>|";
    for (char& c : raw) {
        if (static_cast<unsigned char>(c) < 32 || strchr(kInvalidChars, c)) {
            c = ' ';
        }
    }
    raw = TrimCopy(raw);
    while (raw.find("  ") != std::string::npos) {
        raw.erase(raw.find("  "), 1);
    }
    if (raw.size() > 80) {
        raw.resize(80);
        raw = TrimCopy(raw);
    }
    if (raw.empty()) {
        raw = "Community Preset";
    }
    return raw + ".ini";
}

std::string EnsureUniquePresetFileNameInDirectory(const std::string& dir, const std::string& normalizedName) {
    std::string candidate = normalizedName;
    const std::string base = EndsWithIni(normalizedName)
        ? normalizedName.substr(0, normalizedName.size() - 4)
        : normalizedName;
    for (int suffix = 2; suffix < 1000; ++suffix) {
        const std::string fullPath = JoinPath(dir, candidate);
        if (GetFileAttributesA(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
        char numbered[64] = {};
        sprintf_s(numbered, " (%d).ini", suffix);
        candidate = base + numbered;
    }
    return normalizedName;
}

void EnsureDirectoryChain(const std::string& path) {
    if (path.empty()) {
        return;
    }
    std::string partial;
    for (size_t i = 0; i < path.size(); ++i) {
        partial.push_back(path[i]);
        if (path[i] == '\\' || path[i] == '/') {
            if (partial.size() > 1) {
                CreateDirectoryA(partial.c_str(), nullptr);
            }
        }
    }
    CreateDirectoryA(path.c_str(), nullptr);
}

bool WriteCommunityTempPreset(const char* iniText, std::string& outPath, std::string& outError) {
    outPath.clear();
    const std::string root = JoinPath(GetPresetDirectory(), "CrimsonWeather");
    EnsureDirectoryChain(root);
    const std::string communityDir = JoinPath(root, "community");
    EnsureDirectoryChain(communityDir);
    const std::string tmpDir = JoinPath(communityDir, "tmp");
    EnsureDirectoryChain(tmpDir);

    char tempName[128] = {};
    sprintf_s(tempName, "download-%llu.ini", static_cast<unsigned long long>(GetTickCount64()));
    outPath = JoinPath(tmpDir, tempName);
    FILE* fp = nullptr;
    if (fopen_s(&fp, outPath.c_str(), "wb") != 0 || !fp) {
        outError = "Could not create temporary preset file";
        return false;
    }
    const size_t len = iniText ? strlen(iniText) : 0;
    const bool ok = fwrite(iniText ? iniText : "", 1, len, fp) == len && ferror(fp) == 0;
    fclose(fp);
    if (!ok) {
        outError = "Could not write temporary preset file";
        DeleteFileA(outPath.c_str());
        return false;
    }
    return true;
}

bool Preset_ImportCommunityPresetText(
    const char* title,
    const char* author,
    const char* catalogId,
    const char* sha256,
    const char* updatedAt,
    const char* iniText,
    std::string& outFileName,
    std::string& outError) {
    outFileName.clear();
    outError.clear();
    if (!iniText || !iniText[0]) {
        outError = "Preset text is empty";
        return false;
    }
    if (strlen(iniText) > 65536) {
        outError = "Preset is larger than 64 KiB";
        return false;
    }

    std::string tempPath;
    if (!WriteCommunityTempPreset(iniText, tempPath, outError)) {
        return false;
    }

    WeatherPresetPackage package{};
    if (!LoadPresetPackageInternal(tempPath.c_str(), CurrentPresetFormatOptions(), package)) {
        DeleteFileA(tempPath.c_str());
        outError = "Preset parser rejected the file";
        return false;
    }
    DeleteFileA(tempPath.c_str());

    std::string normalizedName;
    if (!IsValidUserPresetName(SanitizeCommunityPresetFileName(title, author), normalizedName)) {
        outError = "Community preset title could not be converted into a file name";
        return false;
    }
    const std::string communityPresetDir = GetCommunityPresetDirectory();
    EnsureDirectoryChain(communityPresetDir);
    normalizedName = EnsureUniquePresetFileNameInDirectory(communityPresetDir, normalizedName);
    const std::string fullPath = JoinPath(communityPresetDir, normalizedName);
    if (!WritePresetPackageWithCommunityMetadata(fullPath.c_str(), CurrentPresetFormatOptions(), package, catalogId, sha256, updatedAt)) {
        outError = "Could not save imported preset";
        return false;
    }

    RefreshPresetListInternal();
    outFileName = normalizedName;
    GUI_SetStatus(("Community preset downloaded: " + GetPresetDisplayNameFromFileName(normalizedName)).c_str());
    Log("[community] imported preset %s\n", normalizedName.c_str());
    return true;
}

bool Preset_UpdateCommunityPresetText(
    int presetIndex,
    const char* title,
    const char* author,
    const char* catalogId,
    const char* sha256,
    const char* updatedAt,
    const char* iniText,
    std::string& outError) {
    outError.clear();
    Preset_EnsureInitialized();
    if (!Preset_IsCommunityPreset(presetIndex)) {
        outError = "Selected preset is not a community preset";
        return false;
    }
    if (!iniText || !iniText[0]) {
        outError = "Preset text is empty";
        return false;
    }
    if (strlen(iniText) > 65536) {
        outError = "Preset is larger than 64 KiB";
        return false;
    }

    std::string tempPath;
    if (!WriteCommunityTempPreset(iniText, tempPath, outError)) {
        return false;
    }

    WeatherPresetPackage package{};
    if (!LoadPresetPackageInternal(tempPath.c_str(), CurrentPresetFormatOptions(), package)) {
        DeleteFileA(tempPath.c_str());
        outError = "Preset parser rejected the file";
        return false;
    }
    DeleteFileA(tempPath.c_str());

    const std::string fullPath = g_presetItems[presetIndex].fullPath;
    if (!WritePresetPackageWithCommunityMetadata(fullPath.c_str(), CurrentPresetFormatOptions(), package, catalogId, sha256, updatedAt)) {
        outError = "Could not update installed preset";
        return false;
    }

    const bool wasSelected = presetIndex == g_selectedPresetIndex;
    RefreshPresetListInternal();
    if (wasSelected) {
        for (int i = 0; i < static_cast<int>(g_presetItems.size()); ++i) {
            CommunityPresetInstallInfo info{};
            if (Preset_GetCommunityInstallInfo(i, info) && EqualsNoCase(info.catalogId, catalogId ? catalogId : "")) {
                g_selectedPresetIndex = i;
                break;
            }
        }
        RefreshSelectedPresetBaselineFromDisk();
    }
    GUI_SetStatus(("Community preset updated: " + std::string(title && title[0] ? title : "preset")).c_str());
    Log("[community] updated installed preset %s by %s\n", title ? title : "", author ? author : "");
    return true;
}

bool PresetSchedule_IsEnabled() {
    return ScheduleIsEnabled();
}

void PresetSchedule_SetEnabled(bool enabled) {
    ScheduleSetEnabled(enabled);
}

int PresetSchedule_GetTimeSource() {
    return ScheduleGetTimeSource();
}

void PresetSchedule_SetTimeSource(int source) {
    ScheduleSetTimeSource(source);
}

std::vector<PresetScheduleRow> PresetSchedule_BuildRows() {
    return ScheduleBuildRows();
}

PresetScheduleStatus PresetSchedule_GetStatus() {
    return ScheduleGetStatus();
}

bool PresetSchedule_AddEntry(const PresetScheduleEntry& entry) {
    return ScheduleAddEntry(entry);
}

bool PresetSchedule_UpdateEntry(int index, const PresetScheduleEntry& entry) {
    return ScheduleUpdateEntry(index, entry);
}

bool PresetSchedule_DeleteEntry(int index) {
    return ScheduleDeleteEntry(index);
}

bool PresetSchedule_ParseAmPm(const char* text, int& outMinute) {
    return ScheduleParseAmPm(text, outMinute);
}

std::string PresetSchedule_FormatAmPm(int minute) {
    return ScheduleFormatAmPm(minute);
}

int PresetSchedule_DefaultBlendSeconds() {
    return ScheduleDefaultBlendSeconds();
}
void Preset_TryAutoApplyRemembered() {
    if (g_autoApplyRememberedTried) return;
    g_autoApplyRememberedTried = true;
    g_autoApplyRememberedArmed = false;
    g_worldReadyStableStartTick = 0;

    Preset_EnsureInitialized();
    if (g_rememberedPresetName.empty()) {
        Log("[preset] no remembered preset to auto apply\n");
        return;
    }

    for (int i = 0; i < static_cast<int>(g_presetItems.size()); ++i) {
        if (!RememberedPresetMatches(g_presetItems[i], g_rememberedPresetName)) continue;
        if (SelectPresetIndexInternal(i, true, "Preset auto applied: ", "ACTIVATED PRESET: ", "auto-applied")) {
            const std::string rememberedBefore = g_rememberedPresetName;
            if (!EqualsNoCase(g_presetItems[i].fileName, rememberedBefore)) {
                PersistRememberedPresetName(g_presetItems[i].fileName.c_str());
                Log("[preset] normalized remembered preset token '%s' -> '%s'\n",
                    rememberedBefore.c_str(), g_presetItems[i].fileName.c_str());
            }
            GUI_SetStatus(("Preset auto applied: " + g_presetItems[i].displayName).c_str());
            Log("[preset] auto applied %s\n", g_presetItems[i].fileName.c_str());
        }
        return;
    }

    Log("[W] remembered preset not found: %s\n", g_rememberedPresetName.c_str());
}

void Preset_ArmAutoApplyRemembered() {
    LoadRememberedPresetNameOnce();
    g_autoApplyRememberedTried = false;
    g_worldReadyStableStartTick = 0;
    g_autoApplyRememberedArmed = !g_rememberedPresetName.empty();
    if (g_autoApplyRememberedArmed) {
        Log("[preset] pending auto-apply: %s\n", g_rememberedPresetName.c_str());
    } else {
        Log("[preset] no remembered preset armed\n");
    }
}

bool Preset_NeedsWorldTick() {
    EnsureTimeScheduleLoaded();
    if (ScheduleNeedsWorldTick()) {
        return true;
    }

    if (g_autoApplyRememberedArmed && !g_autoApplyRememberedTried) {
        return true;
    }

    const bool hasSelectedRuntimePackage = HasSelectedPresetIndexInternal() && g_selectedPresetBaselineValid;
    const bool hasNewRuntimePackage = !HasSelectedPresetIndexInternal() && g_newPresetDraftActive;
    if (!hasSelectedRuntimePackage && !hasNewRuntimePackage) {
        return false;
    }

    if (g_lastAppliedRegion < 0 ||
        IsPresetRegionId(g_regionBlendFrom) ||
        IsPresetRegionId(g_regionBlendTo)) {
        return true;
    }

    const WeatherPresetPackage& runtimePackage = g_editDraftValid ? g_editDraftPackage : g_selectedPresetBaseline;
    for (int regionId = kPresetRegionGlobal + 1; regionId < kPresetRegionCount; ++regionId) {
        if (runtimePackage.regionEnabled[regionId]) {
            return true;
        }
    }

    return false;
}

void Preset_OnWorldTick(bool worldReady, float dt) {
    const bool scheduleBlending = TimeScheduleRuntimeTick(worldReady);
    if (scheduleBlending) {
        (void)dt;
        return;
    }

    const bool hasSelectedRuntimePackage = HasSelectedPresetIndexInternal() && g_selectedPresetBaselineValid;
    const bool hasNewRuntimePackage = !HasSelectedPresetIndexInternal() && g_newPresetDraftActive;
    if (worldReady && (hasSelectedRuntimePackage || hasNewRuntimePackage)) {
        const WeatherPresetPackage& runtimePackage = g_editDraftValid ? g_editDraftPackage : g_selectedPresetBaseline;
        const int regionId = CurrentMajorRegionForPreset();
        const int previousRegionId = g_regionPreviousMajorId.load();
        const float remaining = g_regionTransitionSeconds.load();
        const bool blendInProgress = g_regionBlendTo == regionId && IsPresetRegionId(g_regionBlendFrom);
        const bool canStartRuntimeApply = Preset_CurrentSlidersMatchAppliedRegion();
        const bool canRuntimeApply = blendInProgress || canStartRuntimeApply;

        if (regionId != g_lastAppliedRegion && canStartRuntimeApply && g_regionBlendTo != regionId) {
            if (remaining > 0.01f && IsPresetRegionId(previousRegionId) && previousRegionId != regionId) {
                g_regionBlendFrom = previousRegionId;
                g_regionBlendTo = regionId;
                g_regionBlendLogBucket = -1;
                Log("[preset] blending region %s -> %s over %.1fs\n",
                    RegionDisplayName(previousRegionId),
                    RegionDisplayName(regionId),
                    kRegionTransitionDurationSeconds);
                LogPresetDataSummary("transition-from-effective", previousRegionId, EffectivePresetDataForRegion(runtimePackage, previousRegionId));
                LogPresetDataSummary("transition-to-effective", regionId, EffectivePresetDataForRegion(runtimePackage, regionId));
            } else {
                const WeatherPresetData data = EffectivePresetDataForRegion(runtimePackage, regionId);
                ApplyPresetData(data);
                g_lastRuntimeAppliedData = data;
                g_lastRuntimeAppliedDataValid = true;
                g_lastAppliedRegion = regionId;
                g_regionBlendFrom = -1;
                g_regionBlendTo = -1;
                g_regionBlendLogBucket = -1;
                Log("[preset] applied %s scope for region %s\n",
                    runtimePackage.regionEnabled[regionId] ? "regional" : "global",
                    RegionDisplayName(regionId));
                LogPresetDataSummary("runtime-region-applied", regionId, data);
            }
        }

        if (g_regionBlendTo == regionId && IsPresetRegionId(g_regionBlendFrom) && canRuntimeApply) {
            const WeatherPresetData fromData = EffectivePresetDataForRegion(runtimePackage, g_regionBlendFrom);
            const WeatherPresetData toData = EffectivePresetDataForRegion(runtimePackage, regionId);
            const float progress = 1.0f - ClampPresetFloat(remaining / kRegionTransitionDurationSeconds, 0.0f, 1.0f);
            const WeatherPresetData blended = BlendPresetData(fromData, toData, progress);
            ApplyPresetData(blended);
            g_lastRuntimeAppliedData = blended;
            g_lastRuntimeAppliedDataValid = true;
            const int progressBucket = min(4, max(0, static_cast<int>(progress * 4.0f)));
            if (kPresetVerboseTestLog && progressBucket != g_regionBlendLogBucket) {
                g_regionBlendLogBucket = progressBucket;
                Log("[preset-test] transition-progress from=%s to=%s progress=%d%% remaining=%.2f\n",
                    RegionDisplayName(g_regionBlendFrom),
                    RegionDisplayName(regionId),
                    progressBucket * 25,
                    remaining);
                LogPresetDataSummary("transition-blended", regionId, blended);
            }
            if (remaining <= 0.01f || progress >= 0.999f) {
                ApplyPresetData(toData);
                g_lastRuntimeAppliedData = toData;
                g_lastAppliedRegion = regionId;
                g_regionBlendFrom = -1;
                g_regionBlendTo = -1;
                g_regionBlendLogBucket = -1;
                Log("[preset] finished region blend into %s\n", RegionDisplayName(regionId));
                LogPresetDataSummary("transition-finished-effective", regionId, toData);
            }
        }
    }

    if (!g_autoApplyRememberedArmed || g_autoApplyRememberedTried) return;

    if (!worldReady) {
        g_worldReadyStableStartTick = 0;
        return;
    }

    (void)dt;

    const ULONGLONG now = GetTickCount64();
    if (g_worldReadyStableStartTick == 0) {
        g_worldReadyStableStartTick = now;
        return;
    }

    const ULONGLONG elapsedMs = now - g_worldReadyStableStartTick;
    if (elapsedMs < kWorldReadyAutoApplyDelayMs) return;
    if (g_timeLayoutReady.load() && !g_timeCurrentHourValid.load()) {
        return;
    }

    Log("[preset] world ready after %.2fs; applying remembered preset\n", elapsedMs / 1000.0);
    Preset_TryAutoApplyRemembered();
}
