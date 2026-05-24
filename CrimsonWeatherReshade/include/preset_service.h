#pragma once

#include <string>
#include <vector>

struct WeatherPresetColor {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;
};

struct WeatherPresetData {
    bool forceClearSky = false;
    bool noRain = false;
    float rain = 0.0f;
    float thunder = 0.0f;
    bool noDust = false;
    float dust = 0.0f;
    bool noSnow = false;
    float snow = 0.0f;
    bool snowAccumBoundaryAEnabled = false;
    float snowAccumBoundaryA = -5.0f;
    bool snowAccumBoundaryBEnabled = false;
    float snowAccumBoundaryB = -20.0f;
    bool snowCoverageThresholdEnabled = false;
    float snowCoverageThreshold = -20.0f;
    bool visualTimeOverride = false;
    bool progressVisualTime = false;
    bool progressVisualTimeMatchGameTime = false;
    float progressVisualTimeIntervalMs = 0.0f;
    float timeHour = 12.0f;
    bool cloudAmountEnabled = false;
    float cloudAmount = 1.0f;
    bool cloudHeightEnabled = false;
    float cloudHeight = 1.0f;
    bool cloudDensityEnabled = false;
    float cloudDensity = 1.0f;
    bool midCloudsEnabled = false;
    float midClouds = 1.0f;
    bool highCloudsEnabled = false;
    float highClouds = 1.0f;
    bool cloudAlphaEnabled = false;
    float cloudAlpha = 1.0f;
    bool cloudFadeRangeEnabled = false;
    float cloudFadeRange = 0.0f;
    bool cloudDetailRatioEnabled = false;
    float cloudDetailRatio = 0.0f;
    bool cloudPhaseFrontEnabled = false;
    float cloudPhaseFront = 0.0f;
    bool cloudScatteringCoefficientEnabled = false;
    float cloudScatteringCoefficient = 0.0f;
    bool cloudFlowEnabled = false;
    float cloudFlow = 1.0f;
    bool cloudVisibleRangeEnabled = false;
    float cloudVisibleRange = 1.0f;
    bool rayleighHeightEnabled = false;
    float rayleighHeight = 1200.0f;
    bool ozoneRatioEnabled = false;
    float ozoneRatio = 0.0f;
    bool rayleighScatteringColorEnabled = false;
    WeatherPresetColor rayleighScatteringColor{};
    bool exp2CEnabled = false;
    float exp2C = 1.0f;
    bool exp2DEnabled = false;
    float exp2D = 1.0f;
    bool cloudVariationEnabled = false;
    float cloudVariation = 1.0f;
    bool nightSkyRotationEnabled = false;
    float nightSkyRotation = 0.0f;
    bool nightSkyYawEnabled = false;
    float nightSkyYaw = 0.0f;
    bool sunSizeEnabled = false;
    float sunSize = 0.267f;
    bool sunLightIntensityEnabled = false;
    float sunLightIntensity = 1.0f;
    bool sunYawEnabled = false;
    float sunYaw = 0.0f;
    bool sunPitchEnabled = false;
    float sunPitch = 0.0f;
    bool moonSizeEnabled = false;
    float moonSize = 0.267f;
    bool moonLightIntensityEnabled = false;
    float moonLightIntensity = 1.0f;
    bool moonYawEnabled = false;
    float moonYaw = 0.0f;
    bool moonPitchEnabled = false;
    float moonPitch = 0.0f;
    bool moonRollEnabled = false;
    float moonRoll = 0.0f;
    bool moonTextureEnabled = false;
    std::string moonTexture;
    bool milkywayTextureEnabled = false;
    std::string milkywayTexture;
    bool fogEnabled = false;
    float fogPercent = 0.0f;
    bool nativeFogEnabled = false;
    float nativeFog = 1.0f;
    bool volumeFogScatterColorEnabled = false;
    WeatherPresetColor volumeFogScatterColor{};
    bool mieScatterColorEnabled = false;
    WeatherPresetColor mieScatterColor{};
    bool mieScaleHeightEnabled = false;
    float mieScaleHeight = 1200.0f;
    bool mieAerosolDensityEnabled = false;
    float mieAerosolDensity = 1.0f;
    bool mieAerosolAbsorptionEnabled = false;
    float mieAerosolAbsorption = 0.0f;
    bool heightFogBaselineEnabled = false;
    float heightFogBaseline = 0.0f;
    bool heightFogFalloffEnabled = false;
    float heightFogFalloff = 0.0f;
    bool noFog = false;
    float wind = 1.0f;
    bool noWind = false;
    bool puddleScaleEnabled = false;
    float puddleScale = 0.0f;
};

constexpr int kPresetRegionGlobal = 0;
constexpr int kPresetRegionHernand = 1;
constexpr int kPresetRegionDemeniss = 2;
constexpr int kPresetRegionDelesyia = 3;
constexpr int kPresetRegionPailune = 4;
constexpr int kPresetRegionCrimsonDesert = 5;
constexpr int kPresetRegionAbyss = 6;
constexpr int kPresetRegionCount = 7;

struct WeatherPresetSourceMask {
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

struct WeatherPresetStatusSnapshot {
    bool hasPresetPackage = false;
    int playerRegion = kPresetRegionGlobal;
    int editRegion = kPresetRegionGlobal;
    int blendFromRegion = -1;
    int blendToRegion = -1;
    float blendProgress = 1.0f;
    WeatherPresetData effective{};
    WeatherPresetSourceMask regionSource{};
};

struct PresetScheduleEntry {
    int startMinute = 0;
    int endMinute = 0;
    std::string presetFile;
    int blendSeconds = 120;
};

struct PresetScheduleRow {
    bool gap = false;
    int startMinute = 0;
    int endMinute = 0;
    int entryIndex = -1;
    std::string presetFile;
    std::string displayName;
    bool presetMissing = false;
    int blendSeconds = 0;
};

struct PresetScheduleStatus {
    bool enabled = false;
    bool active = false;
    int timeSource = 0;
    bool timeSourceValid = false;
    int currentMinute = -1;
    int activeEntryIndex = -1;
    std::string activePresetFile;
    std::string activeDisplayName;
    bool blending = false;
    std::string blendFromDisplayName;
    std::string blendToDisplayName;
    int blendRemainingSeconds = 0;
};

struct CommunityPresetInstallInfo {
    bool valid = false;
    std::string catalogId;
    std::string sha256;
    std::string updatedAt;
    std::string displayName;
    std::string fullPath;
};

void Preset_EnsureInitialized();
void Preset_Refresh();
int Preset_GetCount();
const char* Preset_GetDisplayName(int index);
const char* Preset_GetFileName(int index);
bool Preset_IsCommunityPreset(int index);
bool Preset_GetCommunityInstallInfo(int index, CommunityPresetInstallInfo& outInfo);
int Preset_GetSelectedIndex();
bool Preset_HasSelection();
const char* Preset_GetSelectedDisplayName();
const char* Preset_GetRegionDisplayName(int regionId);
int Preset_GetEditRegion();
void Preset_SetEditRegion(int regionId);
bool Preset_SelectedHasRegion(int regionId);
bool Preset_IsEditingDetachedRegion();
WeatherPresetData Preset_GetEditRegionData();
WeatherPresetSourceMask Preset_GetEditRegionOverrideMask();
void Preset_SetEditRegionData(const WeatherPresetData& data);
void Preset_SetEditRegionDataWithOverrides(const WeatherPresetData& data, const WeatherPresetSourceMask& mask);
void Preset_ResetEditRegion();
void Preset_SelectNew();
bool Preset_SelectIndex(int index);
bool Preset_HasUnsavedChanges();
bool Preset_CanSaveCurrent();
void Preset_AutoSaveTick(bool uiEditActive);
WeatherPresetStatusSnapshot Preset_GetStatusSnapshot();
void Preset_ArmAutoApplyRemembered();
bool Preset_NeedsWorldTick();
void Preset_OnWorldTick(bool worldReady, float dt);
void Preset_TryAutoApplyRemembered();
bool Preset_SaveSelected();
bool Preset_SaveAs(const char* fileName);
bool Preset_ExportCurrentCanonical(std::string& outIni, std::string& outError);
bool Preset_ExportPresetCanonicalByIndex(int index, std::string& outIni, std::string& outError);
bool Preset_ImportCommunityPresetText(
    const char* title,
    const char* author,
    const char* catalogId,
    const char* sha256,
    const char* updatedAt,
    const char* iniText,
    std::string& outFileName,
    std::string& outError);
bool Preset_UpdateCommunityPresetText(
    int presetIndex,
    const char* title,
    const char* author,
    const char* catalogId,
    const char* sha256,
    const char* updatedAt,
    const char* iniText,
    std::string& outError);

bool PresetSchedule_IsEnabled();
void PresetSchedule_SetEnabled(bool enabled);
int PresetSchedule_GetTimeSource();
void PresetSchedule_SetTimeSource(int source);
std::vector<PresetScheduleRow> PresetSchedule_BuildRows();
PresetScheduleStatus PresetSchedule_GetStatus();
bool PresetSchedule_AddEntry(const PresetScheduleEntry& entry);
bool PresetSchedule_UpdateEntry(int index, const PresetScheduleEntry& entry);
bool PresetSchedule_DeleteEntry(int index);
bool PresetSchedule_ParseAmPm(const char* text, int& outMinute);
std::string PresetSchedule_FormatAmPm(int minute);
int PresetSchedule_DefaultBlendSeconds();
