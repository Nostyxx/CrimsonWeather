#include "pch.h"
#include "runtime_shared.h"
#include "preset_service.h"

#include <algorithm>
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

std::vector<PresetListItem> g_presetItems;
bool g_presetsInitialized = false;
int g_selectedPresetIndex = -1;
WeatherPresetData g_selectedPresetBaseline{};
bool g_selectedPresetBaselineValid = false;

constexpr float kPresetFloatEpsilon = 0.0005f;
constexpr const char* kNewPresetDisplayName = "[New Preset]";
constexpr const char* kPresetConfigSection = "Preset";
constexpr const char* kPresetConfigKeyLastPreset = "LastPreset";
constexpr int kPresetFormatVersion = 2;
std::string TrimCopy(const std::string& value);

float ClampPresetFloat(float value, float lo, float hi) {
    return min(hi, max(lo, value));
}

bool HasSelectedPresetIndexInternal() {
    return g_selectedPresetIndex >= 0 && g_selectedPresetIndex < static_cast<int>(g_presetItems.size());
}

void ClearSelectedPresetBaseline() {
    g_selectedPresetBaseline = WeatherPresetData{};
    g_selectedPresetBaselineValid = false;
}

void SetSelectedPresetBaseline(const WeatherPresetData& data) {
    g_selectedPresetBaseline = data;
    g_selectedPresetBaselineValid = true;
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
        a.cloudHeightEnabled == b.cloudHeightEnabled &&
        FloatNearlyEqual(a.cloudHeight, b.cloudHeight) &&
        a.cloudDensityEnabled == b.cloudDensityEnabled &&
        FloatNearlyEqual(a.cloudDensity, b.cloudDensity) &&
        a.midCloudsEnabled == b.midCloudsEnabled &&
        FloatNearlyEqual(a.midClouds, b.midClouds) &&
        a.highCloudsEnabled == b.highCloudsEnabled &&
        FloatNearlyEqual(a.highClouds, b.highClouds) &&
        a.sunLocationXEnabled == b.sunLocationXEnabled &&
        FloatNearlyEqual(a.sunLocationX, b.sunLocationX) &&
        a.sunLocationYEnabled == b.sunLocationYEnabled &&
        FloatNearlyEqual(a.sunLocationY, b.sunLocationY) &&
        a.moonLocationXEnabled == b.moonLocationXEnabled &&
        FloatNearlyEqual(a.moonLocationX, b.moonLocationX) &&
        a.moonLocationYEnabled == b.moonLocationYEnabled &&
        FloatNearlyEqual(a.moonLocationY, b.moonLocationY) &&
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
    bool cloudHeightEnabledSeen = false;
    bool cloudDensityEnabledSeen = false;
    bool midCloudsEnabledSeen = false;
    bool highCloudsEnabledSeen = false;
    bool sunLocationXEnabledSeen = false;
    bool sunLocationYEnabledSeen = false;
    bool moonLocationXEnabledSeen = false;
    bool moonLocationYEnabledSeen = false;
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

void ParsePresetKeyValue(const std::string& key, const std::string& value, PresetParseState& state) {
    bool boolValue = false;
    float floatValue = 0.0f;
    WeatherPresetData& data = state.data;

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
    } else if (KeyEquals(key, "ForceCloudsEnabled")) {
        if (TryParseBool(value, boolValue)) data.forceCloudsEnabled = boolValue;
    } else if (KeyEquals(key, "ForceClouds")) {
        if (TryParseFloat(value, floatValue)) data.forceCloudsPercent = floatValue;
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
    } else if (KeyEquals(key, "SunLocationXEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.sunLocationXEnabled = boolValue;
            state.sunLocationXEnabledSeen = true;
        }
    } else if (KeyEquals(key, "SunLocationX")) {
        if (TryParseFloat(value, floatValue)) data.sunLocationX = floatValue;
    } else if (KeyEquals(key, "SunLocationYEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.sunLocationYEnabled = boolValue;
            state.sunLocationYEnabledSeen = true;
        }
    } else if (KeyEquals(key, "SunLocationY")) {
        if (TryParseFloat(value, floatValue)) data.sunLocationY = floatValue;
    } else if (KeyEquals(key, "MoonLocationXEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.moonLocationXEnabled = boolValue;
            state.moonLocationXEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MoonLocationX")) {
        if (TryParseFloat(value, floatValue)) data.moonLocationX = floatValue;
    } else if (KeyEquals(key, "MoonLocationYEnabled")) {
        if (TryParseBool(value, boolValue)) {
            data.moonLocationYEnabled = boolValue;
            state.moonLocationYEnabledSeen = true;
        }
    } else if (KeyEquals(key, "MoonLocationY")) {
        if (TryParseFloat(value, floatValue)) data.moonLocationY = floatValue;
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

    data.forceCloudsPercent = 0.0f;
    data.forceCloudsEnabled = false;
    if (!state.cloudHeightEnabledSeen) data.cloudHeightEnabled = !FloatNearlyEqual(data.cloudHeight, 1.0f);
    if (!state.cloudDensityEnabledSeen) data.cloudDensityEnabled = !FloatNearlyEqual(data.cloudDensity, 1.0f);
    if (!state.midCloudsEnabledSeen) data.midCloudsEnabled = !FloatNearlyEqual(data.midClouds, 1.0f);
    if (!state.highCloudsEnabledSeen) data.highCloudsEnabled = !FloatNearlyEqual(data.highClouds, 1.0f);
    if (!state.sunLocationXEnabledSeen) data.sunLocationXEnabled = false;
    if (!state.sunLocationYEnabledSeen) data.sunLocationYEnabled = false;
    if (!state.moonLocationXEnabledSeen) data.moonLocationXEnabled = false;
    if (!state.moonLocationYEnabledSeen) data.moonLocationYEnabled = false;
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
    data.sunLocationX = ClampPresetFloat(data.sunLocationX, -180.0f, 180.0f);
    data.sunLocationY = ClampPresetFloat(data.sunLocationY, -180.0f, 180.0f);
    data.moonLocationX = ClampPresetFloat(data.moonLocationX, -180.0f, 180.0f);
    data.moonLocationY = ClampPresetFloat(data.moonLocationY, -180.0f, 180.0f);

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
    AppendPresetKeyValue(out, "CloudHeightEnabled", FormatPresetBool(data.cloudHeightEnabled));
    AppendPresetKeyValue(out, "CloudHeight", FormatPresetFloat(ClampPresetFloat(data.cloudHeight, -20.0f, 20.0f)));
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

WeatherPresetData CaptureCurrentPresetData() {
    WeatherPresetData data{};
    data.forceClearSky = g_forceClear.load();
    data.rain = ActiveOverrideValue(g_oRain, 0.0f);
    data.dust = ActiveOverrideValue(g_oDust, 0.0f);
    data.snow = ActiveOverrideValue(g_oSnow, 0.0f);
    data.visualTimeOverride = g_timeCtrlActive.load() && g_timeFreeze.load();
    data.timeHour = NormalizeHour24(g_timeTargetHour.load());
    data.forceCloudsEnabled = false;
    data.forceCloudsPercent = 0.0f;
    data.cloudHeightEnabled = g_oCloudSpdX.active.load();
    data.cloudHeight = OverrideValueIf(data.cloudHeightEnabled, g_oCloudSpdX, 1.0f);
    data.cloudDensityEnabled = g_oCloudSpdY.active.load();
    data.cloudDensity = OverrideValueIf(data.cloudDensityEnabled, g_oCloudSpdY, 1.0f);
    data.midCloudsEnabled = g_oHighClouds.active.load();
    data.midClouds = OverrideValueIf(data.midCloudsEnabled, g_oHighClouds, 1.0f);
    data.highCloudsEnabled = g_oAtmoAlpha.active.load();
    data.highClouds = OverrideValueIf(data.highCloudsEnabled, g_oAtmoAlpha, 1.0f);
    if (RuntimeFeatureAvailable(RuntimeFeatureId::CelestialControls)) {
        data.sunLocationXEnabled = g_oSunDirX.active.load();
        data.sunLocationX = OverrideValueIf(data.sunLocationXEnabled, g_oSunDirX, 0.0f);
        data.sunLocationYEnabled = g_oSunDirY.active.load();
        data.sunLocationY = OverrideValueIf(data.sunLocationYEnabled, g_oSunDirY, 0.0f);
        data.moonLocationXEnabled = g_oMoonDirX.active.load();
        data.moonLocationX = OverrideValueIf(data.moonLocationXEnabled, g_oMoonDirX, 0.0f);
        data.moonLocationYEnabled = g_oMoonDirY.active.load();
        data.moonLocationY = OverrideValueIf(data.moonLocationYEnabled, g_oMoonDirY, 0.0f);
    }
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

    g_forceCloudsAmount.store(kForceCloudsDefaultAmount);
    g_forceCloudsEnabled.store(false);

    ApplyEnabledOverride(g_oCloudSpdX, data.cloudHeightEnabled, data.cloudHeight, -20.0f, 20.0f);
    ApplyEnabledOverride(g_oCloudSpdY, data.cloudDensityEnabled, data.cloudDensity, 0.0f, 10.0f);
    ApplyEnabledOverride(g_oHighClouds, data.midCloudsEnabled, data.midClouds, 0.0f, 15.0f);
    ApplyEnabledOverride(g_oAtmoAlpha, data.highCloudsEnabled, data.highClouds, 0.0f, 15.0f);
    if (RuntimeFeatureAvailable(RuntimeFeatureId::CelestialControls)) {
        ApplyEnabledOverride(g_oSunDirX, data.sunLocationXEnabled, data.sunLocationX, -180.0f, 180.0f);
        ApplyEnabledOverride(g_oSunDirY, data.sunLocationYEnabled, data.sunLocationY, -180.0f, 180.0f);
        ApplyEnabledOverride(g_oMoonDirX, data.moonLocationXEnabled, data.moonLocationX, -180.0f, 180.0f);
        ApplyEnabledOverride(g_oMoonDirY, data.moonLocationYEnabled, data.moonLocationY, -180.0f, 180.0f);
    } else {
        g_oSunDirX.clear();
        g_oSunDirY.clear();
        g_oMoonDirX.clear();
        g_oMoonDirY.clear();
    }
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

bool LoadPresetFileInternal(const char* path, WeatherPresetData& outData) {
    if (!IsValidPresetFile(path)) return false;
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) return false;

    PresetParseState state{};
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
        if (!text.empty() && text.front() == '[') continue;

        const size_t eq = text.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = TrimCopy(text.substr(0, eq));
        const std::string value = TrimCopy(text.substr(eq + 1));
        ParsePresetKeyValue(key, value, state);
    }

    fclose(fp);
    if (!headerSeen) return false;
    NormalizeLoadedPreset(state, path);
    outData = state.data;
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
    WeatherPresetData data{};
    if (!LoadPresetFileInternal(g_presetItems[g_selectedPresetIndex].fullPath.c_str(), data)) {
        ClearSelectedPresetBaseline();
        return;
    }
    SetSelectedPresetBaseline(data);
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

void Preset_SelectNew() {
    g_selectedPresetIndex = -1;
    ClearSelectedPresetBaseline();
    PersistRememberedPresetName(nullptr);
}

bool Preset_HasUnsavedChanges() {
    if (!Preset_HasSelection()) return true;
    if (!g_selectedPresetBaselineValid) return true;
    const WeatherPresetData current = CaptureCurrentPresetData();
    return !PresetDataEquals(current, g_selectedPresetBaseline);
}

bool Preset_CanSaveCurrent() {
    if (!Preset_HasSelection()) return true;
    return Preset_HasUnsavedChanges();
}

bool Preset_SelectIndex(int index) {
    if (index < 0 || index >= static_cast<int>(g_presetItems.size())) return false;
    WeatherPresetData data{};
    if (!LoadPresetFileInternal(g_presetItems[index].fullPath.c_str(), data)) {
        GUI_SetStatus("Preset load failed");
        Log("[preset] failed to load %s\n", g_presetItems[index].fileName.c_str());
        return false;
    }
    ApplyPresetData(data);
    g_selectedPresetIndex = index;
    SetSelectedPresetBaseline(data);
    PersistRememberedPresetName(g_presetItems[index].fileName.c_str());
    GUI_SetStatus(("Preset loaded: " + g_presetItems[index].displayName).c_str());
    ShowNativeToast(("ACTIVATED PRESET: " + g_presetItems[index].displayName).c_str());
    Log("[preset] loaded %s\n", g_presetItems[index].fileName.c_str());
    return true;
}

bool Preset_SaveSelected() {
    if (!Preset_HasSelection()) return false;
    const WeatherPresetData data = CaptureCurrentPresetData();
    const PresetListItem item = g_presetItems[g_selectedPresetIndex];
    if (!WritePresetFileInternal(item.fullPath.c_str(), data)) {
        GUI_SetStatus("Preset save failed");
        Log("[preset] failed to save %s\n", item.fileName.c_str());
        return false;
    }
    SetSelectedPresetBaseline(data);
    PersistRememberedPresetName(item.fileName.c_str());
    GUI_SetStatus(("Preset saved: " + item.displayName).c_str());
    Log("[preset] saved %s\n", item.fileName.c_str());
    return true;
}

bool Preset_SaveAs(const char* fileName) {
    std::string normalizedName;
    if (!IsValidUserPresetName(fileName ? fileName : "", normalizedName)) {
        GUI_SetStatus("Invalid preset name");
        return false;
    }

    const WeatherPresetData data = CaptureCurrentPresetData();
    const std::string fullPath = JoinPath(GetPresetDirectory(), normalizedName);
    if (!WritePresetFileInternal(fullPath.c_str(), data)) {
        GUI_SetStatus("Preset save failed");
        Log("[preset] failed to save %s\n", normalizedName.c_str());
        return false;
    }

    RefreshPresetListInternal();
    ClearSelectedPresetBaseline();
    for (int i = 0; i < static_cast<int>(g_presetItems.size()); ++i) {
        if (EqualsNoCase(g_presetItems[i].fileName, normalizedName)) {
            g_selectedPresetIndex = i;
            SetSelectedPresetBaseline(data);
            break;
        }
    }
    PersistRememberedPresetName(normalizedName.c_str());

    GUI_SetStatus(("Preset saved: " + GetPresetDisplayNameFromFileName(normalizedName)).c_str());
    Log("[preset] saved %s\n", normalizedName.c_str());
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
