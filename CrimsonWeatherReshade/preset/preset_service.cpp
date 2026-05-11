#include "pch.h"
#include "runtime_shared.h"
#include "preset_service.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
constexpr const char* kPresetHeader = "[CrimsonWeatherPreset]";

struct PresetListItem {
    std::string fileName;
    std::string displayName;
    std::string fullPath;
};

struct WeatherPresetMask {
    bool forceClearSky = false;
    bool rain = false;
    bool dust = false;
    bool snow = false;
    bool time = false;
    bool cloudAmount = false;
    bool cloudHeight = false;
    bool cloudDensity = false;
    bool midClouds = false;
    bool highClouds = false;
    bool exp2C = false;
    bool exp2D = false;
    bool cloudVariation = false;
    bool nightSkyRotation = false;
    bool fog = false;
    bool nativeFog = false;
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

constexpr float kPresetFloatEpsilon = 0.0005f;
constexpr float kRegionTransitionDurationSeconds = 6.0f;
constexpr bool kPresetVerboseTestLog = false;
constexpr const char* kNewPresetDisplayName = "[New Preset]";
constexpr const char* kPresetConfigSection = "Preset";
constexpr const char* kPresetConfigKeyLastPreset = "LastPreset";
constexpr int kPresetFormatVersion = 3;
std::string TrimCopy(const std::string& value);
bool EqualsNoCase(const std::string& a, const std::string& b);
WeatherPresetData CaptureCurrentPresetData();
void ApplyPresetData(const WeatherPresetData& data);

float ClampPresetFloat(float value, float lo, float hi) {
    return min(hi, max(lo, value));
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
}

void SetSelectedPresetBaseline(const WeatherPresetPackage& package) {
    g_selectedPresetBaseline = package;
    g_selectedPresetBaselineValid = true;
    g_editDraftPackage = package;
    g_editDraftValid = true;
    g_newPresetDraftActive = false;
}

bool FloatNearlyEqual(float a, float b, float epsilon = kPresetFloatEpsilon) {
    return std::fabs(a - b) <= epsilon;
}

bool HourNearlyEqual(float a, float b, float epsilon = kPresetFloatEpsilon) {
    const float na = NormalizeHour24(a);
    const float nb = NormalizeHour24(b);
    float delta = std::fabs(na - nb);
    if (delta > 12.0f) delta = 24.0f - delta;
    return delta <= epsilon;
}

bool PresetDataEquals(const WeatherPresetData& a, const WeatherPresetData& b) {
    return a.forceClearSky == b.forceClearSky &&
        FloatNearlyEqual(a.rain, b.rain) &&
        FloatNearlyEqual(a.dust, b.dust) &&
        FloatNearlyEqual(a.snow, b.snow) &&
        a.visualTimeOverride == b.visualTimeOverride &&
        HourNearlyEqual(a.timeHour, b.timeHour) &&
        a.cloudAmountEnabled == b.cloudAmountEnabled &&
        FloatNearlyEqual(a.cloudAmount, b.cloudAmount) &&
        a.cloudHeightEnabled == b.cloudHeightEnabled &&
        FloatNearlyEqual(a.cloudHeight, b.cloudHeight) &&
        a.cloudDensityEnabled == b.cloudDensityEnabled &&
        FloatNearlyEqual(a.cloudDensity, b.cloudDensity) &&
        a.midCloudsEnabled == b.midCloudsEnabled &&
        FloatNearlyEqual(a.midClouds, b.midClouds) &&
        a.highCloudsEnabled == b.highCloudsEnabled &&
        FloatNearlyEqual(a.highClouds, b.highClouds) &&
        a.exp2CEnabled == b.exp2CEnabled &&
        FloatNearlyEqual(a.exp2C, b.exp2C) &&
        a.exp2DEnabled == b.exp2DEnabled &&
        FloatNearlyEqual(a.exp2D, b.exp2D) &&
        a.cloudVariationEnabled == b.cloudVariationEnabled &&
        FloatNearlyEqual(a.cloudVariation, b.cloudVariation) &&
        a.nightSkyRotationEnabled == b.nightSkyRotationEnabled &&
        FloatNearlyEqual(a.nightSkyRotation, b.nightSkyRotation) &&
        a.fogEnabled == b.fogEnabled &&
        FloatNearlyEqual(a.fogPercent, b.fogPercent) &&
        a.nativeFogEnabled == b.nativeFogEnabled &&
        FloatNearlyEqual(a.nativeFog, b.nativeFog) &&
        FloatNearlyEqual(a.wind, b.wind) &&
        a.noWind == b.noWind &&
        a.puddleScaleEnabled == b.puddleScaleEnabled &&
        FloatNearlyEqual(a.puddleScale, b.puddleScale);
}

bool PresetMaskAny(const WeatherPresetMask& mask) {
    return mask.forceClearSky || mask.rain || mask.dust || mask.snow || mask.time ||
        mask.cloudAmount || mask.cloudHeight || mask.cloudDensity || mask.midClouds ||
        mask.highClouds || mask.exp2C || mask.exp2D || mask.cloudVariation ||
        mask.nightSkyRotation || mask.fog || mask.nativeFog || mask.wind ||
        mask.noWind || mask.puddleScale;
}

WeatherPresetSourceMask ToSourceMask(const WeatherPresetMask& mask) {
    WeatherPresetSourceMask out{};
    out.forceClearSky = mask.forceClearSky;
    out.rain = mask.rain;
    out.dust = mask.dust;
    out.snow = mask.snow;
    out.time = mask.time;
    out.cloudAmount = mask.cloudAmount;
    out.cloudHeight = mask.cloudHeight;
    out.cloudDensity = mask.cloudDensity;
    out.midClouds = mask.midClouds;
    out.highClouds = mask.highClouds;
    out.exp2C = mask.exp2C;
    out.exp2D = mask.exp2D;
    out.cloudVariation = mask.cloudVariation;
    out.nightSkyRotation = mask.nightSkyRotation;
    out.fog = mask.fog;
    out.nativeFog = mask.nativeFog;
    out.wind = mask.wind;
    out.noWind = mask.noWind;
    out.puddleScale = mask.puddleScale;
    return out;
}

WeatherPresetMask FromSourceMask(const WeatherPresetSourceMask& source) {
    WeatherPresetMask mask{};
    mask.forceClearSky = source.forceClearSky;
    mask.rain = source.rain;
    mask.dust = source.dust;
    mask.snow = source.snow;
    mask.time = source.time;
    mask.cloudAmount = source.cloudAmount;
    mask.cloudHeight = source.cloudHeight;
    mask.cloudDensity = source.cloudDensity;
    mask.midClouds = source.midClouds;
    mask.highClouds = source.highClouds;
    mask.exp2C = source.exp2C;
    mask.exp2D = source.exp2D;
    mask.cloudVariation = source.cloudVariation;
    mask.nightSkyRotation = source.nightSkyRotation;
    mask.fog = source.fog;
    mask.nativeFog = source.nativeFog;
    mask.wind = source.wind;
    mask.noWind = source.noWind;
    mask.puddleScale = source.puddleScale;
    return mask;
}

bool PresetMaskEquals(const WeatherPresetMask& a, const WeatherPresetMask& b) {
    return a.forceClearSky == b.forceClearSky &&
        a.rain == b.rain &&
        a.dust == b.dust &&
        a.snow == b.snow &&
        a.time == b.time &&
        a.cloudAmount == b.cloudAmount &&
        a.cloudHeight == b.cloudHeight &&
        a.cloudDensity == b.cloudDensity &&
        a.midClouds == b.midClouds &&
        a.highClouds == b.highClouds &&
        a.exp2C == b.exp2C &&
        a.exp2D == b.exp2D &&
        a.cloudVariation == b.cloudVariation &&
        a.nightSkyRotation == b.nightSkyRotation &&
        a.fog == b.fog &&
        a.nativeFog == b.nativeFog &&
        a.wind == b.wind &&
        a.noWind == b.noWind &&
        a.puddleScale == b.puddleScale;
}

WeatherPresetMask BuildFullPresetMask() {
    WeatherPresetMask mask{};
    mask.forceClearSky = true;
    mask.rain = true;
    mask.dust = true;
    mask.snow = true;
    mask.time = true;
    mask.cloudAmount = true;
    mask.cloudHeight = true;
    mask.cloudDensity = true;
    mask.midClouds = true;
    mask.highClouds = true;
    mask.exp2C = true;
    mask.exp2D = true;
    mask.cloudVariation = true;
    mask.nightSkyRotation = true;
    mask.fog = true;
    mask.nativeFog = true;
    mask.wind = true;
    mask.noWind = true;
    mask.puddleScale = true;
    return mask;
}

WeatherPresetMask BuildOverrideMask(const WeatherPresetData& base, const WeatherPresetData& value) {
    WeatherPresetMask mask{};
    mask.forceClearSky = base.forceClearSky != value.forceClearSky;
    mask.rain = !FloatNearlyEqual(base.rain, value.rain);
    mask.dust = !FloatNearlyEqual(base.dust, value.dust);
    mask.snow = !FloatNearlyEqual(base.snow, value.snow);
    mask.time = base.visualTimeOverride != value.visualTimeOverride || !HourNearlyEqual(base.timeHour, value.timeHour);
    mask.cloudAmount = base.cloudAmountEnabled != value.cloudAmountEnabled || !FloatNearlyEqual(base.cloudAmount, value.cloudAmount);
    mask.cloudHeight = base.cloudHeightEnabled != value.cloudHeightEnabled || !FloatNearlyEqual(base.cloudHeight, value.cloudHeight);
    mask.cloudDensity = base.cloudDensityEnabled != value.cloudDensityEnabled || !FloatNearlyEqual(base.cloudDensity, value.cloudDensity);
    mask.midClouds = base.midCloudsEnabled != value.midCloudsEnabled || !FloatNearlyEqual(base.midClouds, value.midClouds);
    mask.highClouds = base.highCloudsEnabled != value.highCloudsEnabled || !FloatNearlyEqual(base.highClouds, value.highClouds);
    mask.exp2C = base.exp2CEnabled != value.exp2CEnabled || !FloatNearlyEqual(base.exp2C, value.exp2C);
    mask.exp2D = base.exp2DEnabled != value.exp2DEnabled || !FloatNearlyEqual(base.exp2D, value.exp2D);
    mask.cloudVariation = base.cloudVariationEnabled != value.cloudVariationEnabled || !FloatNearlyEqual(base.cloudVariation, value.cloudVariation);
    mask.nightSkyRotation = base.nightSkyRotationEnabled != value.nightSkyRotationEnabled || !FloatNearlyEqual(base.nightSkyRotation, value.nightSkyRotation);
    mask.fog = base.fogEnabled != value.fogEnabled || !FloatNearlyEqual(base.fogPercent, value.fogPercent);
    mask.nativeFog = base.nativeFogEnabled != value.nativeFogEnabled || !FloatNearlyEqual(base.nativeFog, value.nativeFog);
    mask.wind = !FloatNearlyEqual(base.wind, value.wind);
    mask.noWind = base.noWind != value.noWind;
    mask.puddleScale = base.puddleScaleEnabled != value.puddleScaleEnabled || !FloatNearlyEqual(base.puddleScale, value.puddleScale);
    return mask;
}

void ApplyPresetMask(WeatherPresetData& target, const WeatherPresetData& source, const WeatherPresetMask& mask) {
    if (mask.forceClearSky) target.forceClearSky = source.forceClearSky;
    if (mask.rain) target.rain = source.rain;
    if (mask.dust) target.dust = source.dust;
    if (mask.snow) target.snow = source.snow;
    if (mask.time) {
        target.visualTimeOverride = source.visualTimeOverride;
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
    if (mask.fog) {
        target.fogEnabled = source.fogEnabled;
        target.fogPercent = source.fogPercent;
    }
    if (mask.nativeFog) {
        target.nativeFogEnabled = source.nativeFogEnabled;
        target.nativeFog = source.nativeFog;
    }
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

float LerpPresetHour(float a, float b, float t) {
    const float na = NormalizeHour24(a);
    const float nb = NormalizeHour24(b);
    float delta = nb - na;
    if (delta > 12.0f) delta -= 24.0f;
    if (delta < -12.0f) delta += 24.0f;
    return NormalizeHour24(na + delta * t);
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
    out.rain = LerpPresetFloat(a.rain, b.rain, t);
    out.dust = LerpPresetFloat(a.dust, b.dust, t);
    out.snow = LerpPresetFloat(a.snow, b.snow, t);
    out.visualTimeOverride = ChoosePresetBool(a.visualTimeOverride, b.visualTimeOverride, t);
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
    out.exp2CEnabled = a.exp2CEnabled || b.exp2CEnabled;
    out.exp2C = LerpPresetFloat(a.exp2C, b.exp2C, t);
    out.exp2DEnabled = a.exp2DEnabled || b.exp2DEnabled;
    out.exp2D = LerpPresetFloat(a.exp2D, b.exp2D, t);
    out.cloudVariationEnabled = a.cloudVariationEnabled || b.cloudVariationEnabled;
    out.cloudVariation = LerpPresetFloat(a.cloudVariation, b.cloudVariation, t);
    out.nightSkyRotationEnabled = a.nightSkyRotationEnabled || b.nightSkyRotationEnabled;
    out.nightSkyRotation = LerpPresetFloat(a.nightSkyRotation, b.nightSkyRotation, t);
    out.fogEnabled = a.fogEnabled || b.fogEnabled;
    out.fogPercent = LerpPresetFloat(a.fogPercent, b.fogPercent, t);
    out.nativeFogEnabled = a.nativeFogEnabled || b.nativeFogEnabled;
    out.nativeFog = LerpPresetFloat(a.nativeFog, b.nativeFog, t);
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

void AppendMaskField(std::string& out, bool enabled, const char* name) {
    if (!enabled) return;
    if (!out.empty()) out += ",";
    out += name;
}

std::string PresetMaskSummary(const WeatherPresetMask& mask) {
    std::string out;
    AppendMaskField(out, mask.forceClearSky, "clear");
    AppendMaskField(out, mask.rain, "rain");
    AppendMaskField(out, mask.dust, "dust");
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
    AppendMaskField(out, mask.nightSkyRotation, "nightRot");
    AppendMaskField(out, mask.fog, "fog");
    AppendMaskField(out, mask.nativeFog, "nativeFog");
    AppendMaskField(out, mask.wind, "wind");
    AppendMaskField(out, mask.noWind, "noWind");
    AppendMaskField(out, mask.puddleScale, "puddle");
    return out.empty() ? std::string("<none>") : out;
}

void LogPresetDataSummary(const char* tag, int regionId, const WeatherPresetData& data) {
    if (!kPresetVerboseTestLog) return;
    Log("[preset-test] %s region=%s clear=%d rain=%.3f dust=%.3f snow=%.3f time=%d@%.2f wind=%.3f noWind=%d fog=%d/%.1f nativeFog=%d/%.2f cloudAmt=%d/%.2f cloudH=%d/%.2f cloudD=%d/%.2f puddle=%d/%.3f\n",
        tag ? tag : "data",
        RegionDisplayName(regionId),
        data.forceClearSky ? 1 : 0,
        data.rain,
        data.dust,
        data.snow,
        data.visualTimeOverride ? 1 : 0,
        NormalizeHour24(data.timeHour),
        data.wind,
        data.noWind ? 1 : 0,
        data.fogEnabled ? 1 : 0,
        data.fogPercent,
        data.nativeFogEnabled ? 1 : 0,
        data.nativeFog,
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
float g_worldReadyStableSeconds = 0.0f;
constexpr float kWorldReadyAutoApplyDelaySeconds = 1.0f;

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

bool RememberedPresetMatches(const PresetListItem& item, const std::string& rememberedRaw) {
    if (rememberedRaw.empty()) return false;
    const std::string trimmed = TrimCopy(rememberedRaw);
    if (trimmed.empty()) return false;
    if (EqualsNoCase(item.fileName, trimmed)) return true;
    if (EqualsNoCase(item.displayName, trimmed)) return true;
    return EqualsNoCase(item.fileName, EnsureIniExtension(trimmed));
}

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

struct PresetParseState {
    WeatherPresetData data{};
    WeatherPresetMask mask{};
    bool cloudAmountEnabledSeen = false;
    bool cloudHeightEnabledSeen = false;
    bool cloudDensityEnabledSeen = false;
    bool midCloudsEnabledSeen = false;
    bool highCloudsEnabledSeen = false;
    bool exp2CEnabledSeen = false;
    bool exp2DEnabledSeen = false;
    bool cloudVariationEnabledSeen = false;
    bool nightSkyRotationEnabledSeen = false;
    bool fogEnabledSeen = false;
    bool nativeFogEnabledSeen = false;
    bool puddleScaleEnabledSeen = false;
    bool sawLegacyAlias = false;
};

bool KeyEquals(const std::string& key, const char* expected) {
    return _stricmp(key.c_str(), expected) == 0;
}

void MarkPresetMaskForKey(const std::string& key, WeatherPresetMask& mask) {
    if (KeyEquals(key, "ForceClearSky")) mask.forceClearSky = true;
    else if (KeyEquals(key, "Rain")) mask.rain = true;
    else if (KeyEquals(key, "Dust")) mask.dust = true;
    else if (KeyEquals(key, "Snow")) mask.snow = true;
    else if (KeyEquals(key, "VisualTimeOverride") || KeyEquals(key, "TimeHour")) mask.time = true;
    else if (KeyEquals(key, "CloudAmountEnabled") || KeyEquals(key, "CloudAmount")) mask.cloudAmount = true;
    else if (KeyEquals(key, "CloudHeightEnabled") || KeyEquals(key, "CloudHeight")) mask.cloudHeight = true;
    else if (KeyEquals(key, "CloudDensityEnabled") || KeyEquals(key, "CloudDensity")) mask.cloudDensity = true;
    else if (KeyEquals(key, "MidCloudsEnabled") || KeyEquals(key, "MidClouds") ||
             KeyEquals(key, "HighCloudsEnabled") || KeyEquals(key, "HighClouds") ||
             KeyEquals(key, "CloudScrollEnabled") || KeyEquals(key, "CloudScroll")) mask.midClouds = true;
    else if (KeyEquals(key, "HighCloudLayerEnabled") || KeyEquals(key, "HighCloudLayer")) mask.highClouds = true;
    else if (KeyEquals(key, "2CEnabled") || KeyEquals(key, "2C")) mask.exp2C = true;
    else if (KeyEquals(key, "2DEnabled") || KeyEquals(key, "2D")) mask.exp2D = true;
    else if (KeyEquals(key, "CloudVariationEnabled") || KeyEquals(key, "CloudVariation") ||
             KeyEquals(key, "CloudThicknessEnabled") || KeyEquals(key, "CloudThickness")) mask.cloudVariation = true;
    else if (KeyEquals(key, "NightSkyRotationEnabled") || KeyEquals(key, "NightSkyRotation")) mask.nightSkyRotation = true;
    else if (KeyEquals(key, "FogEnabled") || KeyEquals(key, "Fog")) mask.fog = true;
    else if (KeyEquals(key, "NativeFogEnabled") || KeyEquals(key, "NativeFog") ||
             KeyEquals(key, "PlainFogEnabled") || KeyEquals(key, "PlainFog")) mask.nativeFog = true;
    else if (KeyEquals(key, "Wind")) mask.wind = true;
    else if (KeyEquals(key, "NoWind")) mask.noWind = true;
    else if (KeyEquals(key, "PuddleScaleEnabled") || KeyEquals(key, "PuddleScale")) mask.puddleScale = true;
}

void ParsePresetKeyValue(const std::string& key, const std::string& value, PresetParseState& state) {
    bool boolValue = false;
    float floatValue = 0.0f;
    WeatherPresetData& data = state.data;
    MarkPresetMaskForKey(key, state.mask);

    if (KeyEquals(key, "ForceClearSky")) {
        if (TryParseBool(value, boolValue)) data.forceClearSky = boolValue;
    } else if (KeyEquals(key, "Rain")) {
        if (TryParseFloat(value, floatValue)) data.rain = floatValue;
    } else if (KeyEquals(key, "Dust")) {
        if (TryParseFloat(value, floatValue)) data.dust = floatValue;
    } else if (KeyEquals(key, "Snow")) {
        if (TryParseFloat(value, floatValue)) data.snow = floatValue;
    } else if (KeyEquals(key, "VisualTimeOverride")) {
        if (TryParseBool(value, boolValue)) data.visualTimeOverride = boolValue;
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
    } else if (KeyEquals(key, "NightSkyRotationEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.nightSkyRotationEnabled = boolValue;
            state.nightSkyRotationEnabledSeen = true;
        }
    } else if (KeyEquals(key, "NightSkyRotation")) {
        if (TryParseFloat(value, floatValue)) data.nightSkyRotation = floatValue;
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
    }
}

void NormalizeLoadedPreset(PresetParseState& state, const char* path) {
    WeatherPresetData& data = state.data;

    if (!state.cloudAmountEnabledSeen) data.cloudAmountEnabled = !FloatNearlyEqual(data.cloudAmount, 1.0f);
    if (!state.cloudHeightEnabledSeen) data.cloudHeightEnabled = !FloatNearlyEqual(data.cloudHeight, 1.0f);
    if (!state.cloudDensityEnabledSeen) data.cloudDensityEnabled = !FloatNearlyEqual(data.cloudDensity, 1.0f);
    if (!state.midCloudsEnabledSeen) data.midCloudsEnabled = !FloatNearlyEqual(data.midClouds, 1.0f);
    if (!state.highCloudsEnabledSeen) data.highCloudsEnabled = !FloatNearlyEqual(data.highClouds, 1.0f);
    if (!state.exp2CEnabledSeen) data.exp2CEnabled = false;
    if (!state.exp2DEnabledSeen) data.exp2DEnabled = false;
    if (!state.cloudVariationEnabledSeen) data.cloudVariationEnabled = false;
    if (!state.nightSkyRotationEnabledSeen) data.nightSkyRotationEnabled = false;
    if (!state.fogEnabledSeen) data.fogEnabled = !FloatNearlyEqual(data.fogPercent, 0.0f);
    if (!state.nativeFogEnabledSeen) data.nativeFogEnabled = !FloatNearlyEqual(data.nativeFog, 0.0f);
    if (!state.puddleScaleEnabledSeen) data.puddleScaleEnabled = !FloatNearlyEqual(data.puddleScale, 0.0f);

    data.exp2C = ClampPresetFloat(data.exp2C, 0.0f, 15.0f);
    data.exp2D = ClampPresetFloat(data.exp2D, 0.0f, 15.0f);
    data.nightSkyRotation = ClampPresetFloat(data.nightSkyRotation, -15.0f, 15.0f);
    data.nativeFog = ClampPresetFloat(data.nativeFog, 0.0f, 15.0f);
    data.cloudAmount = ClampPresetFloat(data.cloudAmount, 0.0f, 15.0f);

    if (state.sawLegacyAlias) {
        Log("[preset] loaded legacy cloud aliases from %s\n", path);
    }
}

float ActiveOverrideValue(const SliderOverride& overrideValue, float inactiveValue) {
    return overrideValue.active.load() ? overrideValue.value.load() : inactiveValue;
}

float OverrideValueIf(bool enabled, const SliderOverride& overrideValue, float inactiveValue) {
    return enabled ? overrideValue.value.load() : inactiveValue;
}

void ApplyEnabledOverride(SliderOverride& overrideValue, bool enabled, float value, float lo, float hi) {
    if (enabled) {
        overrideValue.set(ClampPresetFloat(value, lo, hi));
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

std::string FormatPresetBool(bool value) {
    return value ? "1" : "0";
}

std::string FormatPresetFloat(float value) {
    char buf[64] = {};
    sprintf_s(buf, "%.4f", value);
    return buf;
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

std::string SerializeCanonicalPreset(const WeatherPresetData& data) {
    std::string out;
    out.reserve(768);

    AppendPresetLine(out, kPresetHeader);
    AppendPresetLine(out, "[Meta]");
    AppendPresetKeyValue(out, "FormatVersion", std::to_string(kPresetFormatVersion));
    out += '\n';

    AppendPresetLine(out, "[Weather]");
    AppendPresetKeyValue(out, "ForceClearSky", FormatPresetBool(data.forceClearSky));
    AppendPresetKeyValue(out, "Rain", FormatPresetFloat(Clamp01(data.rain)));
    AppendPresetKeyValue(out, "Dust", FormatPresetFloat(ClampPresetFloat(data.dust, 0.0f, 2.0f)));
    AppendPresetKeyValue(out, "Snow", FormatPresetFloat(Clamp01(data.snow)));
    out += '\n';

    AppendPresetLine(out, "[Time]");
    AppendPresetKeyValue(out, "VisualTimeOverride", FormatPresetBool(data.visualTimeOverride));
    AppendPresetKeyValue(out, "TimeHour", FormatPresetFloat(NormalizeHour24(data.timeHour)));
    out += '\n';

    AppendPresetLine(out, "[Cloud]");
    AppendPresetKeyValue(out, "CloudAmountEnabled", FormatPresetBool(data.cloudAmountEnabled));
    AppendPresetKeyValue(out, "CloudAmount", FormatPresetFloat(ClampPresetFloat(data.cloudAmount, 0.0f, 15.0f)));
    AppendPresetKeyValue(out, "CloudHeightEnabled", FormatPresetBool(data.cloudHeightEnabled));
    AppendPresetKeyValue(out, "CloudHeight", FormatPresetFloat(ClampPresetFloat(data.cloudHeight, -15.0f, 15.0f)));
    AppendPresetKeyValue(out, "CloudDensityEnabled", FormatPresetBool(data.cloudDensityEnabled));
    AppendPresetKeyValue(out, "CloudDensity", FormatPresetFloat(ClampPresetFloat(data.cloudDensity, 0.0f, 10.0f)));
    AppendPresetKeyValue(out, "MidCloudsEnabled", FormatPresetBool(data.midCloudsEnabled));
    AppendPresetKeyValue(out, "MidClouds", FormatPresetFloat(ClampPresetFloat(data.midClouds, 0.0f, 15.0f)));
    AppendPresetKeyValue(out, "HighCloudLayerEnabled", FormatPresetBool(data.highCloudsEnabled));
    AppendPresetKeyValue(out, "HighCloudLayer", FormatPresetFloat(ClampPresetFloat(data.highClouds, 0.0f, 15.0f)));
    out += '\n';

    AppendPresetLine(out, "[Experiment]");
    AppendPresetKeyValue(out, "2CEnabled", FormatPresetBool(data.exp2CEnabled));
    AppendPresetKeyValue(out, "2C", FormatPresetFloat(ClampPresetFloat(data.exp2C, 0.0f, 15.0f)));
    AppendPresetKeyValue(out, "2DEnabled", FormatPresetBool(data.exp2DEnabled));
    AppendPresetKeyValue(out, "2D", FormatPresetFloat(ClampPresetFloat(data.exp2D, 0.0f, 15.0f)));
    AppendPresetKeyValue(out, "CloudVariationEnabled", FormatPresetBool(data.cloudVariationEnabled));
    AppendPresetKeyValue(out, "CloudVariation", FormatPresetFloat(ClampPresetFloat(data.cloudVariation, 0.0f, 15.0f)));
    AppendPresetKeyValue(out, "NightSkyRotationEnabled", FormatPresetBool(data.nightSkyRotationEnabled));
    AppendPresetKeyValue(out, "NightSkyRotation", FormatPresetFloat(ClampPresetFloat(data.nightSkyRotation, -15.0f, 15.0f)));
    AppendPresetKeyValue(out, "PuddleScaleEnabled", FormatPresetBool(data.puddleScaleEnabled));
    AppendPresetKeyValue(out, "PuddleScale", FormatPresetFloat(Clamp01(data.puddleScale)));
    out += '\n';

    AppendPresetLine(out, "[Atmosphere]");
    AppendPresetKeyValue(out, "FogEnabled", FormatPresetBool(data.fogEnabled));
    AppendPresetKeyValue(out, "Fog", FormatPresetFloat(ClampPresetFloat(data.fogPercent, 0.0f, 100.0f)));
    AppendPresetKeyValue(out, "NativeFogEnabled", FormatPresetBool(data.nativeFogEnabled));
    AppendPresetKeyValue(out, "NativeFog", FormatPresetFloat(ClampPresetFloat(data.nativeFog, 0.0f, 15.0f)));
    AppendPresetKeyValue(out, "Wind", FormatPresetFloat(ClampPresetFloat(data.wind, 0.0f, 15.0f)));
    AppendPresetKeyValue(out, "NoWind", FormatPresetBool(data.noWind));

    return out;
}

void AppendRegionSectionHeader(std::string& out, int regionId, const char* section) {
    out += "[Region.";
    out += RegionToken(regionId);
    out += ".";
    out += section;
    out += "]\n";
}

void AppendMaskedRegionPresetData(std::string& out, int regionId, const WeatherPresetData& data, const WeatherPresetMask& mask) {
    if (mask.forceClearSky || mask.rain || mask.dust || mask.snow) {
        AppendRegionSectionHeader(out, regionId, "Weather");
        if (mask.forceClearSky) AppendPresetKeyValue(out, "ForceClearSky", FormatPresetBool(data.forceClearSky));
        if (mask.rain) AppendPresetKeyValue(out, "Rain", FormatPresetFloat(Clamp01(data.rain)));
        if (mask.dust) AppendPresetKeyValue(out, "Dust", FormatPresetFloat(ClampPresetFloat(data.dust, 0.0f, 2.0f)));
        if (mask.snow) AppendPresetKeyValue(out, "Snow", FormatPresetFloat(Clamp01(data.snow)));
        out += '\n';
    }

    if (mask.time) {
        AppendRegionSectionHeader(out, regionId, "Time");
        AppendPresetKeyValue(out, "VisualTimeOverride", FormatPresetBool(data.visualTimeOverride));
        AppendPresetKeyValue(out, "TimeHour", FormatPresetFloat(NormalizeHour24(data.timeHour)));
        out += '\n';
    }

    if (mask.cloudAmount || mask.cloudHeight || mask.cloudDensity || mask.midClouds || mask.highClouds) {
        AppendRegionSectionHeader(out, regionId, "Cloud");
        if (mask.cloudAmount) {
            AppendPresetKeyValue(out, "CloudAmountEnabled", FormatPresetBool(data.cloudAmountEnabled));
            AppendPresetKeyValue(out, "CloudAmount", FormatPresetFloat(ClampPresetFloat(data.cloudAmount, 0.0f, 15.0f)));
        }
        if (mask.cloudHeight) {
            AppendPresetKeyValue(out, "CloudHeightEnabled", FormatPresetBool(data.cloudHeightEnabled));
            AppendPresetKeyValue(out, "CloudHeight", FormatPresetFloat(ClampPresetFloat(data.cloudHeight, -15.0f, 15.0f)));
        }
        if (mask.cloudDensity) {
            AppendPresetKeyValue(out, "CloudDensityEnabled", FormatPresetBool(data.cloudDensityEnabled));
            AppendPresetKeyValue(out, "CloudDensity", FormatPresetFloat(ClampPresetFloat(data.cloudDensity, 0.0f, 10.0f)));
        }
        if (mask.midClouds) {
            AppendPresetKeyValue(out, "MidCloudsEnabled", FormatPresetBool(data.midCloudsEnabled));
            AppendPresetKeyValue(out, "MidClouds", FormatPresetFloat(ClampPresetFloat(data.midClouds, 0.0f, 15.0f)));
        }
        if (mask.highClouds) {
            AppendPresetKeyValue(out, "HighCloudLayerEnabled", FormatPresetBool(data.highCloudsEnabled));
            AppendPresetKeyValue(out, "HighCloudLayer", FormatPresetFloat(ClampPresetFloat(data.highClouds, 0.0f, 15.0f)));
        }
        out += '\n';
    }

    if (mask.exp2C || mask.exp2D || mask.cloudVariation || mask.nightSkyRotation || mask.puddleScale) {
        AppendRegionSectionHeader(out, regionId, "Experiment");
        if (mask.exp2C) {
            AppendPresetKeyValue(out, "2CEnabled", FormatPresetBool(data.exp2CEnabled));
            AppendPresetKeyValue(out, "2C", FormatPresetFloat(ClampPresetFloat(data.exp2C, 0.0f, 15.0f)));
        }
        if (mask.exp2D) {
            AppendPresetKeyValue(out, "2DEnabled", FormatPresetBool(data.exp2DEnabled));
            AppendPresetKeyValue(out, "2D", FormatPresetFloat(ClampPresetFloat(data.exp2D, 0.0f, 15.0f)));
        }
        if (mask.cloudVariation) {
            AppendPresetKeyValue(out, "CloudVariationEnabled", FormatPresetBool(data.cloudVariationEnabled));
            AppendPresetKeyValue(out, "CloudVariation", FormatPresetFloat(ClampPresetFloat(data.cloudVariation, 0.0f, 15.0f)));
        }
        if (mask.nightSkyRotation) {
            AppendPresetKeyValue(out, "NightSkyRotationEnabled", FormatPresetBool(data.nightSkyRotationEnabled));
            AppendPresetKeyValue(out, "NightSkyRotation", FormatPresetFloat(ClampPresetFloat(data.nightSkyRotation, -15.0f, 15.0f)));
        }
        if (mask.puddleScale) {
            AppendPresetKeyValue(out, "PuddleScaleEnabled", FormatPresetBool(data.puddleScaleEnabled));
            AppendPresetKeyValue(out, "PuddleScale", FormatPresetFloat(Clamp01(data.puddleScale)));
        }
        out += '\n';
    }

    if (mask.fog || mask.nativeFog || mask.wind || mask.noWind) {
        AppendRegionSectionHeader(out, regionId, "Atmosphere");
        if (mask.fog) {
            AppendPresetKeyValue(out, "FogEnabled", FormatPresetBool(data.fogEnabled));
            AppendPresetKeyValue(out, "Fog", FormatPresetFloat(ClampPresetFloat(data.fogPercent, 0.0f, 100.0f)));
        }
        if (mask.nativeFog) {
            AppendPresetKeyValue(out, "NativeFogEnabled", FormatPresetBool(data.nativeFogEnabled));
            AppendPresetKeyValue(out, "NativeFog", FormatPresetFloat(ClampPresetFloat(data.nativeFog, 0.0f, 15.0f)));
        }
        if (mask.wind) AppendPresetKeyValue(out, "Wind", FormatPresetFloat(ClampPresetFloat(data.wind, 0.0f, 15.0f)));
        if (mask.noWind) AppendPresetKeyValue(out, "NoWind", FormatPresetBool(data.noWind));
        out += '\n';
    }
}

std::string SerializePresetPackage(const WeatherPresetPackage& package) {
    std::string out = SerializeCanonicalPreset(package.global);
    for (int regionId = 1; regionId < kPresetRegionCount; ++regionId) {
        if (!package.regionEnabled[regionId]) continue;
        out += "\n[Region.";
        out += RegionToken(regionId);
        out += "]\n";
        AppendPresetKeyValue(out, "Enabled", "1");
        out += '\n';
        AppendMaskedRegionPresetData(out, regionId, package.region[regionId], package.regionMask[regionId]);
    }
    return out;
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

WeatherPresetData CaptureCurrentPresetData() {
    WeatherPresetData data{};
    data.forceClearSky = g_forceClear.load();
    data.rain = ActiveOverrideValue(g_oRain, 0.0f);
    data.dust = ActiveOverrideValue(g_oDust, 0.0f);
    data.snow = ActiveOverrideValue(g_oSnow, 0.0f);
    data.visualTimeOverride = g_timeCtrlActive.load() && g_timeFreeze.load();
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
    data.exp2CEnabled = g_oExpCloud2C.active.load();
    data.exp2C = OverrideValueIf(data.exp2CEnabled, g_oExpCloud2C, 1.0f);
    data.exp2DEnabled = g_oExpCloud2D.active.load();
    data.exp2D = OverrideValueIf(data.exp2DEnabled, g_oExpCloud2D, 1.0f);
    data.cloudVariationEnabled = g_oCloudVariation.active.load();
    data.cloudVariation = OverrideValueIf(data.cloudVariationEnabled, g_oCloudVariation, 1.0f);
    data.nightSkyRotationEnabled = g_oExpNightSkyRot.active.load();
    data.nightSkyRotation = OverrideValueIf(data.nightSkyRotationEnabled, g_oExpNightSkyRot, 1.0f);
    data.fogEnabled = g_oFog.active.load();
    if (data.fogEnabled) {
        const float fogN = sqrtf(min(1.0f, max(0.0f, g_oFog.value.load() / 100.0f)));
        data.fogPercent = fogN * 100.0f;
    } else {
        data.fogPercent = 0.0f;
    }
    data.nativeFogEnabled = g_oNativeFog.active.load();
    data.nativeFog = data.nativeFogEnabled ? ClampPresetFloat(g_oNativeFog.value.load(), 0.0f, 15.0f) : 0.0f;
    data.wind = ClampPresetFloat(g_windMul.load(), 0.0f, 15.0f);
    data.noWind = g_noWind.load();
    data.puddleScaleEnabled = g_oCloudThk.active.load();
    data.puddleScale = OverrideValueIf(data.puddleScaleEnabled, g_oCloudThk, 0.0f);
    return data;
}

void ApplyPresetData(const WeatherPresetData& data) {
    g_forceClear.store(data.forceClearSky);

    ApplyPositiveOverride(g_oRain, data.rain, 0.0f, 1.0f);
    ApplyPositiveOverride(g_oDust, data.dust, 0.0f, 2.0f);
    ApplyPositiveOverride(g_oSnow, data.snow, 0.0f, 1.0f);

    g_timeTargetHour.store(NormalizeHour24(data.timeHour));
    if (data.visualTimeOverride && g_timeLayoutReady.load()) {
        g_timeCtrlActive.store(true);
        g_timeFreeze.store(true);
        g_timeApplyRequest.store(true);
    } else {
        g_timeFreeze.store(false);
        g_timeCtrlActive.store(false);
        g_timeApplyRequest.store(true);
        if (data.visualTimeOverride && !g_timeLayoutReady.load()) {
            Log("[W] preset visual time skipped: time layout unresolved\n");
        }
    }

    ApplyEnabledOverride(g_oCloudAmount, data.cloudAmountEnabled, data.cloudAmount, 0.0f, 15.0f);
    ApplyEnabledOverride(g_oCloudSpdX, data.cloudHeightEnabled, data.cloudHeight, -15.0f, 15.0f);
    ApplyEnabledOverride(g_oCloudSpdY, data.cloudDensityEnabled, data.cloudDensity, 0.0f, 10.0f);
    ApplyEnabledOverride(g_oHighClouds, data.midCloudsEnabled, data.midClouds, 0.0f, 15.0f);
    ApplyEnabledOverride(g_oAtmoAlpha, data.highCloudsEnabled, data.highClouds, 0.0f, 15.0f);
    ApplyEnabledOverride(g_oExpCloud2C, data.exp2CEnabled, data.exp2C, 0.0f, 15.0f);
    ApplyEnabledOverride(g_oExpCloud2D, data.exp2DEnabled, data.exp2D, 0.0f, 15.0f);
    ApplyEnabledOverride(g_oCloudVariation, data.cloudVariationEnabled, data.cloudVariation, 0.0f, 15.0f);
    ApplyEnabledOverride(g_oExpNightSkyRot, data.nightSkyRotationEnabled, data.nightSkyRotation, -15.0f, 15.0f);

    const float fogPct = ClampPresetFloat(data.fogPercent, 0.0f, 100.0f);
    if (data.fogEnabled) {
        const float t = fogPct * 0.01f;
        const float fogBoost = t * t * 100.0f;
        g_oFog.set(fogBoost);
    } else g_oFog.clear();

    const float nativeFog = ClampPresetFloat(data.nativeFog, 0.0f, 15.0f);
    if (data.nativeFogEnabled && nativeFog > 0.0001f) g_oNativeFog.set(nativeFog);
    else g_oNativeFog.clear();

    const float wind = ClampPresetFloat(data.wind, 0.0f, 15.0f);
    g_windMul.store(wind);
    g_noWind.store(data.noWind);
    ApplyEnabledOverride(g_oCloudThk, data.puddleScaleEnabled, data.puddleScale, 0.0f, 1.0f);
}

bool LoadPresetPackageInternal(const char* path, WeatherPresetPackage& outPackage) {
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
    WeatherPresetPackage package{};
    NormalizeLoadedPreset(globalState, path);
    package.global = globalState.data;
    for (int regionId = 1; regionId < kPresetRegionCount; ++regionId) {
        if (!regionSeen[regionId] || !regionEnabled[regionId]) continue;
        NormalizeLoadedPreset(regionStates[regionId], path);
        package.region[regionId] = regionStates[regionId].data;
        package.regionMask[regionId] = regionStates[regionId].mask;
        package.regionEnabled[regionId] = PresetMaskAny(package.regionMask[regionId]);
    }
    outPackage = package;
    return true;
}

bool LoadPresetFileInternal(const char* path, WeatherPresetData& outData) {
    WeatherPresetPackage package{};
    if (!LoadPresetPackageInternal(path, package)) return false;
    outData = package.global;
    return true;
}

bool WritePresetFileInternal(const char* path, const WeatherPresetData& data) {
    const std::string serialized = SerializeCanonicalPreset(data);

    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || !fp) return false;
    const size_t bytesWritten = fwrite(serialized.data(), 1, serialized.size(), fp);
    const bool ok = bytesWritten == serialized.size() && ferror(fp) == 0;
    fclose(fp);
    return ok;
}

bool WritePresetPackageInternal(const char* path, const WeatherPresetPackage& package) {
    const std::string serialized = SerializePresetPackage(package);

    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "wb") != 0 || !fp) return false;
    const size_t bytesWritten = fwrite(serialized.data(), 1, serialized.size(), fp);
    const bool ok = bytesWritten == serialized.size() && ferror(fp) == 0;
    fclose(fp);
    return ok;
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
    const std::string dir = GetPresetDirectory();
    WIN32_FIND_DATAA fd{};
    HANDLE find = FindFirstFileA(JoinPath(dir, "*.ini").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            const std::string fileName = fd.cFileName;
            if (!EndsWithIni(fileName)) continue;
            const std::string fullPath = JoinPath(dir, fileName);
            if (!IsValidPresetFile(fullPath.c_str())) continue;
            foundItems.push_back({ fileName, GetPresetDisplayNameFromFileName(fileName), fullPath });
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }

    std::sort(foundItems.begin(), foundItems.end(), [](const PresetListItem& a, const PresetListItem& b) {
        return _stricmp(a.fileName.c_str(), b.fileName.c_str()) < 0;
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
    if (!LoadPresetPackageInternal(g_presetItems[g_selectedPresetIndex].fullPath.c_str(), package)) {
        ClearSelectedPresetBaseline();
        return;
    }
    SetSelectedPresetBaseline(package);
}
} // namespace

void Preset_EnsureInitialized() {
    if (g_presetsInitialized) return;
    LoadRememberedPresetNameOnce();
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
    const bool affectsRuntime = EditRegionAffectsCurrentRuntime();
    SaveEditedRegionToPackage(g_editDraftPackage, g_presetEditRegion, data);
    if (affectsRuntime) {
        ApplyDetectedRegionFromPackage(g_editDraftPackage);
    }
}

void Preset_SetEditRegionDataWithOverrides(const WeatherPresetData& data, const WeatherPresetSourceMask& mask) {
    EnsureEditDraft();
    if (!HasSelectedPresetIndexInternal()) {
        g_newPresetDraftActive = true;
    }
    const bool affectsRuntime = EditRegionAffectsCurrentRuntime();
    SaveEditedRegionToPackageWithMask(g_editDraftPackage, g_presetEditRegion, data, FromSourceMask(mask));
    if (affectsRuntime) {
        ApplyDetectedRegionFromPackage(g_editDraftPackage);
    }
}

void Preset_ResetEditRegion() {
    EnsureEditDraft();
    if (!HasSelectedPresetIndexInternal()) {
        g_newPresetDraftActive = true;
    }
    if (g_presetEditRegion > kPresetRegionGlobal && g_presetEditRegion < kPresetRegionCount) {
        ResetRegionToDefaultsInPackage(g_editDraftPackage, g_presetEditRegion);
        const bool affectsRuntime = EditRegionAffectsCurrentRuntime();
        if (!affectsRuntime) {
            Log("[preset] reset deferred until player enters %s\n", RegionDisplayName(g_presetEditRegion));
        }
        if (affectsRuntime) {
            ApplyDetectedRegionFromPackage(g_editDraftPackage);
        }
        GUI_SetStatus(("Region overrides cleared: " + std::string(RegionDisplayName(g_presetEditRegion))).c_str());
        return;
    }

    SaveEditedRegionToPackage(g_editDraftPackage, kPresetRegionGlobal, WeatherPresetData{});
    ApplyDetectedRegionFromPackage(g_editDraftPackage);
    GUI_SetStatus("Global reset to defaults");
}

void Preset_SelectNew() {
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

bool Preset_SelectIndex(int index) {
    if (index < 0 || index >= static_cast<int>(g_presetItems.size())) return false;
    WeatherPresetPackage package{};
    if (!LoadPresetPackageInternal(g_presetItems[index].fullPath.c_str(), package)) {
        GUI_SetStatus("Preset load failed");
        Log("[preset] failed to load %s\n", g_presetItems[index].fileName.c_str());
        return false;
    }
    ApplyDetectedRegionFromPackage(package);
    g_selectedPresetIndex = index;
    SetSelectedPresetBaseline(package);
    PersistRememberedPresetName(g_presetItems[index].fileName.c_str());
    GUI_SetStatus(("Preset loaded: " + g_presetItems[index].displayName).c_str());
    ShowNativeToast(("ACTIVATED PRESET: " + g_presetItems[index].displayName).c_str());
    Log("[preset] loaded %s\n", g_presetItems[index].fileName.c_str());
    LogPresetPackageSummary("loaded-package", package);
    return true;
}

bool Preset_SaveSelected() {
    if (!Preset_HasSelection()) return false;
    WeatherPresetPackage package = BuildEditDraftPreview();
    const PresetListItem item = g_presetItems[g_selectedPresetIndex];
    if (!WritePresetPackageInternal(item.fullPath.c_str(), package)) {
        GUI_SetStatus("Preset save failed");
        Log("[preset] failed to save %s\n", item.fileName.c_str());
        return false;
    }
    SetSelectedPresetBaseline(package);
    PersistRememberedPresetName(item.fileName.c_str());
    ApplyDetectedRegionFromPackage(package);
    GUI_SetStatus(("Preset saved: " + item.displayName).c_str());
    Log("[preset] saved %s\n", item.fileName.c_str());
    LogPresetPackageSummary("saved-package", package);
    return true;
}

bool Preset_SaveAs(const char* fileName) {
    std::string normalizedName;
    if (!IsValidUserPresetName(fileName ? fileName : "", normalizedName)) {
        GUI_SetStatus("Invalid preset name");
        return false;
    }

    WeatherPresetPackage package = BuildEditDraftPreview();
    const std::string fullPath = JoinPath(GetPresetDirectory(), normalizedName);
    if (!WritePresetPackageInternal(fullPath.c_str(), package)) {
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

void Preset_TryAutoApplyRemembered() {
    if (g_autoApplyRememberedTried) return;
    g_autoApplyRememberedTried = true;
    g_autoApplyRememberedArmed = false;
    g_worldReadyStableSeconds = 0.0f;

    Preset_EnsureInitialized();
    if (g_rememberedPresetName.empty()) {
        Log("[preset] no remembered preset to auto apply\n");
        return;
    }

    for (int i = 0; i < static_cast<int>(g_presetItems.size()); ++i) {
        if (!RememberedPresetMatches(g_presetItems[i], g_rememberedPresetName)) continue;
        if (Preset_SelectIndex(i)) {
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
    g_worldReadyStableSeconds = 0.0f;
    g_autoApplyRememberedArmed = !g_rememberedPresetName.empty();
    if (g_autoApplyRememberedArmed) {
        Log("[preset] pending auto-apply: %s\n", g_rememberedPresetName.c_str());
    } else {
        Log("[preset] no remembered preset armed\n");
    }
}

void Preset_OnWorldTick(bool worldReady, float dt) {
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
        g_worldReadyStableSeconds = 0.0f;
        return;
    }

    if (!(dt > 0.0f) || dt > 0.5f) return;

    g_worldReadyStableSeconds += dt;
    if (g_worldReadyStableSeconds < kWorldReadyAutoApplyDelaySeconds) return;

    Log("[preset] world ready after %.2fs; applying remembered preset\n", g_worldReadyStableSeconds);
    Preset_TryAutoApplyRemembered();
}
