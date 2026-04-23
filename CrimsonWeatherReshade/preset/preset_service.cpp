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
        a.nightSkyRotationEnabled == b.nightSkyRotationEnabled &&
        FloatNearlyEqual(a.nightSkyRotation, b.nightSkyRotation) &&
        a.fogEnabled == b.fogEnabled &&
        FloatNearlyEqual(a.fogPercent, b.fogPercent) &&
        a.plainFogEnabled == b.plainFogEnabled &&
        FloatNearlyEqual(a.plainFog, b.plainFog) &&
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

enum class PresetDocLineKind {
    Raw,
    Header,
    Section,
    KeyValue,
};

struct PresetDocLine {
    PresetDocLineKind kind = PresetDocLineKind::Raw;
    std::string text;
    std::string section;
    std::string key;
    std::string value;
};

struct PresetDocument {
    std::vector<PresetDocLine> lines;
    bool headerSeen = false;
};

struct PresetWriteEntry {
    std::string section;
    std::string key;
    std::string value;
    std::vector<std::string> aliases;
    std::vector<std::string> legacySections;
};

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

void StripLineEnding(std::string& text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
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

PresetDocLine MakeRawPresetLine(const std::string& text) {
    PresetDocLine line{};
    line.kind = PresetDocLineKind::Raw;
    line.text = text;
    return line;
}

PresetDocLine MakeHeaderPresetLine() {
    PresetDocLine line{};
    line.kind = PresetDocLineKind::Header;
    line.text = kPresetHeader;
    return line;
}

PresetDocLine MakeSectionPresetLine(const std::string& section) {
    PresetDocLine line{};
    line.kind = PresetDocLineKind::Section;
    line.section = section;
    line.text = "[" + section + "]";
    return line;
}

PresetDocLine MakeKeyValuePresetLine(const std::string& section, const std::string& key, const std::string& value) {
    PresetDocLine line{};
    line.kind = PresetDocLineKind::KeyValue;
    line.section = section;
    line.key = key;
    line.value = value;
    line.text = key + "=" + value;
    return line;
}

bool ParsePresetDocument(const char* path, PresetDocument& outDoc) {
    outDoc = {};
    if (!path || !path[0]) return false;

    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) return false;

    std::string currentSection;
    bool firstLine = true;
    char lineBuf[512] = {};
    while (fgets(lineBuf, static_cast<int>(sizeof(lineBuf)), fp)) {
        std::string raw = lineBuf;
        StripLineEnding(raw);
        if (firstLine) {
            StripUtf8Bom(raw);
            firstLine = false;
        }

        std::string trimmed = TrimCopy(raw);
        PresetDocLine line = MakeRawPresetLine(raw);
        if (trimmed == kPresetHeader) {
            line.kind = PresetDocLineKind::Header;
            line.text = kPresetHeader;
            outDoc.headerSeen = true;
        } else if (!trimmed.empty() && trimmed.front() == '[' && trimmed.back() == ']') {
            currentSection = TrimCopy(trimmed.substr(1, trimmed.size() - 2));
            line.kind = PresetDocLineKind::Section;
            line.section = currentSection;
            line.text = raw;
        } else {
            const size_t eq = trimmed.find('=');
            if (eq != std::string::npos) {
                line.kind = PresetDocLineKind::KeyValue;
                line.section = currentSection;
                line.key = TrimCopy(trimmed.substr(0, eq));
                line.value = TrimCopy(trimmed.substr(eq + 1));
            }
        }
        outDoc.lines.push_back(std::move(line));
    }

    fclose(fp);
    return outDoc.headerSeen;
}

std::vector<PresetWriteEntry> BuildPresetWriteEntries(const WeatherPresetData& data) {
    std::vector<PresetWriteEntry> entries;
    entries.reserve(26);

    const auto push = [&](const char* section,
                          const char* key,
                          std::string value,
                          std::vector<std::string> aliases = {},
                          std::vector<std::string> legacySections = {}) {
        entries.push_back(PresetWriteEntry{
            section,
            key,
            std::move(value),
            std::move(aliases),
            std::move(legacySections)
        });
    };

    push("Meta", "FormatVersion", std::to_string(kPresetFormatVersion));
    push("Weather", "ForceClearSky", FormatPresetBool(data.forceClearSky));
    push("Weather", "Rain", FormatPresetFloat(Clamp01(data.rain)));
    push("Weather", "Dust", FormatPresetFloat(min(2.0f, max(0.0f, data.dust))));
    push("Weather", "Snow", FormatPresetFloat(Clamp01(data.snow)));

    push("Time", "VisualTimeOverride", FormatPresetBool(data.visualTimeOverride));
    push("Time", "TimeHour", FormatPresetFloat(NormalizeHour24(data.timeHour)));

    push("Cloud", "CloudHeightEnabled", FormatPresetBool(data.cloudHeightEnabled));
    push("Cloud", "CloudHeight", FormatPresetFloat(min(20.0f, max(-20.0f, data.cloudHeight))));
    push("Cloud", "CloudDensityEnabled", FormatPresetBool(data.cloudDensityEnabled));
    push("Cloud", "CloudDensity", FormatPresetFloat(min(10.0f, max(0.0f, data.cloudDensity))));
    push("Cloud", "MidCloudsEnabled", FormatPresetBool(data.midCloudsEnabled),
         { "HighCloudsEnabled", "CloudScrollEnabled" });
    push("Cloud", "MidClouds", FormatPresetFloat(min(15.0f, max(0.0f, data.midClouds))),
         { "HighClouds", "CloudScroll" });
    push("Cloud", "HighCloudLayerEnabled", FormatPresetBool(data.highCloudsEnabled));
    push("Cloud", "HighCloudLayer", FormatPresetFloat(min(15.0f, max(0.0f, data.highClouds))));
    push("Experiment", "2CEnabled", FormatPresetBool(data.exp2CEnabled));
    push("Experiment", "2C", FormatPresetFloat(min(15.0f, max(0.0f, data.exp2C))));
    push("Experiment", "2DEnabled", FormatPresetBool(data.exp2DEnabled));
    push("Experiment", "2D", FormatPresetFloat(min(15.0f, max(0.0f, data.exp2D))));
    push("Experiment", "NightSkyRotationEnabled", FormatPresetBool(data.nightSkyRotationEnabled));
    push("Experiment", "NightSkyRotation", FormatPresetFloat(min(15.0f, max(-15.0f, data.nightSkyRotation))));
    push("Experiment", "PuddleScaleEnabled", FormatPresetBool(data.puddleScaleEnabled), {}, { "Cloud" });
    push("Experiment", "PuddleScale", FormatPresetFloat(Clamp01(data.puddleScale)), {}, { "Cloud" });

    push("Atmosphere", "FogEnabled", FormatPresetBool(data.fogEnabled));
    push("Atmosphere", "Fog", FormatPresetFloat(min(100.0f, max(0.0f, data.fogPercent))));
    push("Atmosphere", "PlainFogEnabled", FormatPresetBool(data.plainFogEnabled));
    push("Atmosphere", "PlainFog", FormatPresetFloat(min(15.0f, max(0.0f, data.plainFog))));
    push("Atmosphere", "Wind", FormatPresetFloat(min(15.0f, max(0.0f, data.wind))));
    push("Atmosphere", "NoWind", FormatPresetBool(data.noWind));

    return entries;
}

bool PresetDocKeyMatches(const PresetDocLine& line, const PresetWriteEntry& entry) {
    if (line.kind != PresetDocLineKind::KeyValue) return false;
    bool sectionMatch = EqualsNoCase(line.section, entry.section);
    if (!sectionMatch) {
        for (const std::string& legacySection : entry.legacySections) {
            if (EqualsNoCase(line.section, legacySection)) {
                sectionMatch = true;
                break;
            }
        }
    }
    if (!sectionMatch) return false;
    if (EqualsNoCase(line.key, entry.key)) return true;
    for (const std::string& alias : entry.aliases) {
        if (EqualsNoCase(line.key, alias)) return true;
    }
    return false;
}

int FindPresetSectionHeaderIndex(const PresetDocument& doc, const std::string& section) {
    for (int i = 0; i < static_cast<int>(doc.lines.size()); ++i) {
        const PresetDocLine& line = doc.lines[i];
        if (line.kind == PresetDocLineKind::Section && EqualsNoCase(line.section, section)) {
            return i;
        }
    }
    return -1;
}

int FindPresetSectionInsertIndex(const PresetDocument& doc, const std::string& section) {
    const int headerIndex = FindPresetSectionHeaderIndex(doc, section);
    if (headerIndex < 0) return -1;

    int insertIndex = headerIndex + 1;
    for (int i = headerIndex + 1; i < static_cast<int>(doc.lines.size()); ++i) {
        if (doc.lines[i].kind == PresetDocLineKind::Section) break;
        insertIndex = i + 1;
    }
    return insertIndex;
}

void EnsurePresetSectionExists(PresetDocument& doc, const std::string& section) {
    if (FindPresetSectionHeaderIndex(doc, section) >= 0) return;
    if (!doc.lines.empty() && !doc.lines.back().text.empty()) {
        doc.lines.push_back(MakeRawPresetLine(""));
    }
    doc.lines.push_back(MakeSectionPresetLine(section));
}

void MovePresetSectionBlockToTop(PresetDocument& doc, const std::string& section) {
    if (doc.lines.empty()) return;

    const int headerIndex = 0;
    const int sectionIndex = FindPresetSectionHeaderIndex(doc, section);
    if (sectionIndex < 0 || sectionIndex == headerIndex + 1) return;

    int blockEnd = sectionIndex + 1;
    while (blockEnd < static_cast<int>(doc.lines.size()) &&
           doc.lines[blockEnd].kind != PresetDocLineKind::Section) {
        ++blockEnd;
    }

    std::vector<PresetDocLine> block(
        doc.lines.begin() + sectionIndex,
        doc.lines.begin() + blockEnd);

    doc.lines.erase(doc.lines.begin() + sectionIndex, doc.lines.begin() + blockEnd);

    int insertIndex = min<int>(1, static_cast<int>(doc.lines.size()));
    doc.lines.insert(doc.lines.begin() + insertIndex, block.begin(), block.end());
}

void MergePresetDocumentWithData(PresetDocument& doc, const WeatherPresetData& data) {
    if (doc.lines.empty() || doc.lines.front().kind != PresetDocLineKind::Header) {
        doc.lines.insert(doc.lines.begin(), MakeHeaderPresetLine());
    }
    doc.headerSeen = true;

    const auto entries = BuildPresetWriteEntries(data);
    for (const PresetWriteEntry& entry : entries) {
        std::vector<int> matchIndices;
        for (int i = 0; i < static_cast<int>(doc.lines.size()); ++i) {
            if (PresetDocKeyMatches(doc.lines[i], entry)) {
                matchIndices.push_back(i);
            }
        }

        if (!matchIndices.empty()) {
            PresetDocLine& first = doc.lines[matchIndices.front()];
            first.kind = PresetDocLineKind::KeyValue;
            first.section = entry.section;
            first.key = entry.key;
            first.value = entry.value;
            first.text = entry.key + "=" + entry.value;

            for (int i = static_cast<int>(matchIndices.size()) - 1; i >= 1; --i) {
                doc.lines.erase(doc.lines.begin() + matchIndices[i]);
            }
            continue;
        }

        EnsurePresetSectionExists(doc, entry.section);
        int insertIndex = FindPresetSectionInsertIndex(doc, entry.section);
        if (insertIndex < 0) {
            doc.lines.push_back(MakeSectionPresetLine(entry.section));
            insertIndex = static_cast<int>(doc.lines.size());
        }
        doc.lines.insert(doc.lines.begin() + insertIndex, MakeKeyValuePresetLine(entry.section, entry.key, entry.value));
    }

    MovePresetSectionBlockToTop(doc, "Meta");
}

std::string SerializePresetDocument(const PresetDocument& doc) {
    std::string out;
    for (size_t i = 0; i < doc.lines.size(); ++i) {
        out += doc.lines[i].text;
        out += '\n';
    }
    return out;
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
    AppendPresetKeyValue(out, "Dust", FormatPresetFloat(min(2.0f, max(0.0f, data.dust))));
    AppendPresetKeyValue(out, "Snow", FormatPresetFloat(Clamp01(data.snow)));
    out += '\n';

    AppendPresetLine(out, "[Time]");
    AppendPresetKeyValue(out, "VisualTimeOverride", FormatPresetBool(data.visualTimeOverride));
    AppendPresetKeyValue(out, "TimeHour", FormatPresetFloat(NormalizeHour24(data.timeHour)));
    out += '\n';

    AppendPresetLine(out, "[Cloud]");
    AppendPresetKeyValue(out, "CloudHeightEnabled", FormatPresetBool(data.cloudHeightEnabled));
    AppendPresetKeyValue(out, "CloudHeight", FormatPresetFloat(min(20.0f, max(-20.0f, data.cloudHeight))));
    AppendPresetKeyValue(out, "CloudDensityEnabled", FormatPresetBool(data.cloudDensityEnabled));
    AppendPresetKeyValue(out, "CloudDensity", FormatPresetFloat(min(10.0f, max(0.0f, data.cloudDensity))));
    AppendPresetKeyValue(out, "MidCloudsEnabled", FormatPresetBool(data.midCloudsEnabled));
    AppendPresetKeyValue(out, "MidClouds", FormatPresetFloat(min(15.0f, max(0.0f, data.midClouds))));
    AppendPresetKeyValue(out, "HighCloudLayerEnabled", FormatPresetBool(data.highCloudsEnabled));
    AppendPresetKeyValue(out, "HighCloudLayer", FormatPresetFloat(min(15.0f, max(0.0f, data.highClouds))));
    out += '\n';

    AppendPresetLine(out, "[Experiment]");
    AppendPresetKeyValue(out, "2CEnabled", FormatPresetBool(data.exp2CEnabled));
    AppendPresetKeyValue(out, "2C", FormatPresetFloat(min(15.0f, max(0.0f, data.exp2C))));
    AppendPresetKeyValue(out, "2DEnabled", FormatPresetBool(data.exp2DEnabled));
    AppendPresetKeyValue(out, "2D", FormatPresetFloat(min(15.0f, max(0.0f, data.exp2D))));
    AppendPresetKeyValue(out, "NightSkyRotationEnabled", FormatPresetBool(data.nightSkyRotationEnabled));
    AppendPresetKeyValue(out, "NightSkyRotation", FormatPresetFloat(min(15.0f, max(-15.0f, data.nightSkyRotation))));
    AppendPresetKeyValue(out, "PuddleScaleEnabled", FormatPresetBool(data.puddleScaleEnabled));
    AppendPresetKeyValue(out, "PuddleScale", FormatPresetFloat(Clamp01(data.puddleScale)));
    out += '\n';

    AppendPresetLine(out, "[Atmosphere]");
    AppendPresetKeyValue(out, "FogEnabled", FormatPresetBool(data.fogEnabled));
    AppendPresetKeyValue(out, "Fog", FormatPresetFloat(min(100.0f, max(0.0f, data.fogPercent))));
    AppendPresetKeyValue(out, "PlainFogEnabled", FormatPresetBool(data.plainFogEnabled));
    AppendPresetKeyValue(out, "PlainFog", FormatPresetFloat(min(15.0f, max(0.0f, data.plainFog))));
    AppendPresetKeyValue(out, "Wind", FormatPresetFloat(min(15.0f, max(0.0f, data.wind))));
    AppendPresetKeyValue(out, "NoWind", FormatPresetBool(data.noWind));

    return out;
}

WeatherPresetData CaptureCurrentPresetData() {
    WeatherPresetData data{};
    data.forceClearSky = g_forceClear.load();
    data.rain = g_oRain.active.load() ? g_oRain.value.load() : 0.0f;
    data.dust = g_oDust.active.load() ? g_oDust.value.load() : 0.0f;
    data.snow = g_oSnow.active.load() ? g_oSnow.value.load() : 0.0f;
    data.visualTimeOverride = g_timeCtrlActive.load() && g_timeFreeze.load();
    data.timeHour = NormalizeHour24(g_timeTargetHour.load());
    data.forceCloudsEnabled = false;
    data.forceCloudsPercent = 0.0f;
    data.cloudHeightEnabled = g_oCloudSpdX.active.load();
    data.cloudHeight = data.cloudHeightEnabled ? g_oCloudSpdX.value.load() : 1.0f;
    data.cloudDensityEnabled = g_oCloudSpdY.active.load();
    data.cloudDensity = data.cloudDensityEnabled ? g_oCloudSpdY.value.load() : 1.0f;
    data.midCloudsEnabled = g_oHighClouds.active.load();
    data.midClouds = data.midCloudsEnabled ? g_oHighClouds.value.load() : 1.0f;
    data.highCloudsEnabled = g_oAtmoAlpha.active.load();
    data.highClouds = data.highCloudsEnabled ? g_oAtmoAlpha.value.load() : 1.0f;
    if (RuntimeFeatureAvailable(RuntimeFeatureId::CelestialControls)) {
        data.sunLocationXEnabled = g_oSunDirX.active.load();
        data.sunLocationX = data.sunLocationXEnabled ? g_oSunDirX.value.load() : 0.0f;
        data.sunLocationYEnabled = g_oSunDirY.active.load();
        data.sunLocationY = data.sunLocationYEnabled ? g_oSunDirY.value.load() : 0.0f;
        data.moonLocationXEnabled = g_oMoonDirX.active.load();
        data.moonLocationX = data.moonLocationXEnabled ? g_oMoonDirX.value.load() : 0.0f;
        data.moonLocationYEnabled = g_oMoonDirY.active.load();
        data.moonLocationY = data.moonLocationYEnabled ? g_oMoonDirY.value.load() : 0.0f;
    }
    data.exp2CEnabled = g_oExpCloud2C.active.load();
    data.exp2C = data.exp2CEnabled ? g_oExpCloud2C.value.load() : 1.0f;
    data.exp2DEnabled = g_oExpCloud2D.active.load();
    data.exp2D = data.exp2DEnabled ? g_oExpCloud2D.value.load() : 1.0f;
    data.nightSkyRotationEnabled = g_oExpNightSkyRot.active.load();
    data.nightSkyRotation = data.nightSkyRotationEnabled ? g_oExpNightSkyRot.value.load() : 1.0f;
    data.fogEnabled = g_oFog.active.load();
    if (data.fogEnabled) {
        const float fogN = sqrtf(min(1.0f, max(0.0f, g_oFog.value.load() / 100.0f)));
        data.fogPercent = fogN * 100.0f;
    } else {
        data.fogPercent = 0.0f;
    }
    data.plainFogEnabled = g_oWind.active.load();
    data.plainFog = data.plainFogEnabled ? min(15.0f, max(0.0f, g_oWind.value.load())) : 0.0f;
    data.wind = min(15.0f, max(0.0f, g_windMul.load()));
    data.noWind = g_noWind.load();
    data.puddleScaleEnabled = g_oCloudThk.active.load();
    data.puddleScale = data.puddleScaleEnabled ? g_oCloudThk.value.load() : 0.0f;
    return data;
}

void ApplyPresetData(const WeatherPresetData& data) {
    g_forceClear.store(data.forceClearSky);

    const float rain = Clamp01(data.rain);
    if (rain > 0.0001f) {
        g_oRain.set(rain);
    } else {
        g_oRain.clear();
    }

    const float dust = min(2.0f, max(0.0f, data.dust));
    if (dust > 0.0001f) g_oDust.set(dust);
    else g_oDust.clear();

    const float snow = Clamp01(data.snow);
    if (snow > 0.0001f) g_oSnow.set(snow);
    else g_oSnow.clear();

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

    if (data.cloudHeightEnabled) g_oCloudSpdX.set(min(20.0f, max(-20.0f, data.cloudHeight)));
    else g_oCloudSpdX.clear();
    if (data.cloudDensityEnabled) g_oCloudSpdY.set(min(10.0f, max(0.0f, data.cloudDensity)));
    else g_oCloudSpdY.clear();
    if (data.midCloudsEnabled) g_oHighClouds.set(min(15.0f, max(0.0f, data.midClouds)));
    else g_oHighClouds.clear();
    if (data.highCloudsEnabled) g_oAtmoAlpha.set(min(15.0f, max(0.0f, data.highClouds)));
    else g_oAtmoAlpha.clear();
    if (RuntimeFeatureAvailable(RuntimeFeatureId::CelestialControls)) {
        if (data.sunLocationXEnabled) g_oSunDirX.set(min(180.0f, max(-180.0f, data.sunLocationX)));
        else g_oSunDirX.clear();
        if (data.sunLocationYEnabled) g_oSunDirY.set(min(180.0f, max(-180.0f, data.sunLocationY)));
        else g_oSunDirY.clear();
        if (data.moonLocationXEnabled) g_oMoonDirX.set(min(180.0f, max(-180.0f, data.moonLocationX)));
        else g_oMoonDirX.clear();
        if (data.moonLocationYEnabled) g_oMoonDirY.set(min(180.0f, max(-180.0f, data.moonLocationY)));
        else g_oMoonDirY.clear();
    } else {
        g_oSunDirX.clear();
        g_oSunDirY.clear();
        g_oMoonDirX.clear();
        g_oMoonDirY.clear();
    }
    if (data.exp2CEnabled) g_oExpCloud2C.set(min(15.0f, max(0.0f, data.exp2C)));
    else g_oExpCloud2C.clear();
    if (data.exp2DEnabled) g_oExpCloud2D.set(min(15.0f, max(0.0f, data.exp2D)));
    else g_oExpCloud2D.clear();
    if (data.nightSkyRotationEnabled) g_oExpNightSkyRot.set(min(15.0f, max(-15.0f, data.nightSkyRotation)));
    else g_oExpNightSkyRot.clear();

    const float fogPct = min(100.0f, max(0.0f, data.fogPercent));
    if (data.fogEnabled) {
        const float t = fogPct * 0.01f;
        const float fogBoost = t * t * 100.0f;
        g_oFog.set(fogBoost);
    } else g_oFog.clear();

    const float plainFog = min(15.0f, max(0.0f, data.plainFog));
    if (data.plainFogEnabled && plainFog > 0.0001f) g_oWind.set(plainFog);
    else g_oWind.clear();

    const float wind = min(15.0f, max(0.0f, data.wind));
    g_windMul.store(wind);
    g_noWind.store(data.noWind);
    if (data.puddleScaleEnabled) g_oCloudThk.set(Clamp01(data.puddleScale));
    else g_oCloudThk.clear();
}

bool LoadPresetFileInternal(const char* path, WeatherPresetData& outData) {
    if (!IsValidPresetFile(path)) return false;
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) return false;

    WeatherPresetData data{};
    bool forceCloudsEnabledSeen = false;
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
    bool nightSkyRotationEnabledSeen = false;
    bool fogEnabledSeen = false;
    bool plainFogEnabledSeen = false;
    bool puddleScaleEnabledSeen = false;
    bool sawLegacyAlias = false;
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
        bool boolValue = false;
        float floatValue = 0.0f;

        if (_stricmp(key.c_str(), "ForceClearSky") == 0) {
            if (TryParseBool(value, boolValue)) data.forceClearSky = boolValue;
        } else if (_stricmp(key.c_str(), "Rain") == 0) {
            if (TryParseFloat(value, floatValue)) data.rain = floatValue;
        } else if (_stricmp(key.c_str(), "Dust") == 0) {
            if (TryParseFloat(value, floatValue)) data.dust = floatValue;
        } else if (_stricmp(key.c_str(), "Snow") == 0) {
            if (TryParseFloat(value, floatValue)) data.snow = floatValue;
        } else if (_stricmp(key.c_str(), "VisualTimeOverride") == 0) {
            if (TryParseBool(value, boolValue)) data.visualTimeOverride = boolValue;
        } else if (_stricmp(key.c_str(), "TimeHour") == 0) {
            if (TryParseFloat(value, floatValue)) data.timeHour = floatValue;
        } else if (_stricmp(key.c_str(), "ForceCloudsEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.forceCloudsEnabled = boolValue;
                forceCloudsEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "ForceClouds") == 0) {
            if (TryParseFloat(value, floatValue)) data.forceCloudsPercent = floatValue;
        } else if (_stricmp(key.c_str(), "CloudHeightEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.cloudHeightEnabled = boolValue;
                cloudHeightEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "CloudHeight") == 0) {
            if (TryParseFloat(value, floatValue)) data.cloudHeight = floatValue;
        } else if (_stricmp(key.c_str(), "CloudDensityEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.cloudDensityEnabled = boolValue;
                cloudDensityEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "CloudDensity") == 0) {
            if (TryParseFloat(value, floatValue)) data.cloudDensity = floatValue;
        } else if (_stricmp(key.c_str(), "MidCloudsEnabled") == 0 ||
                   _stricmp(key.c_str(), "HighCloudsEnabled") == 0 ||
                   _stricmp(key.c_str(), "CloudScrollEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.midCloudsEnabled = boolValue;
                midCloudsEnabledSeen = true;
                if (_stricmp(key.c_str(), "MidCloudsEnabled") != 0) sawLegacyAlias = true;
            }
        } else if (_stricmp(key.c_str(), "MidClouds") == 0 ||
                   _stricmp(key.c_str(), "HighClouds") == 0 ||
                   _stricmp(key.c_str(), "CloudScroll") == 0) {
            if (TryParseFloat(value, floatValue)) {
                data.midClouds = floatValue;
                if (_stricmp(key.c_str(), "MidClouds") != 0) sawLegacyAlias = true;
            }
        } else if (_stricmp(key.c_str(), "HighCloudLayerEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.highCloudsEnabled = boolValue;
                highCloudsEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "HighCloudLayer") == 0) {
            if (TryParseFloat(value, floatValue)) data.highClouds = floatValue;
        } else if (_stricmp(key.c_str(), "SunLocationXEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.sunLocationXEnabled = boolValue;
                sunLocationXEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "SunLocationX") == 0) {
            if (TryParseFloat(value, floatValue)) data.sunLocationX = floatValue;
        } else if (_stricmp(key.c_str(), "SunLocationYEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.sunLocationYEnabled = boolValue;
                sunLocationYEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "SunLocationY") == 0) {
            if (TryParseFloat(value, floatValue)) data.sunLocationY = floatValue;
        } else if (_stricmp(key.c_str(), "MoonLocationXEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.moonLocationXEnabled = boolValue;
                moonLocationXEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "MoonLocationX") == 0) {
            if (TryParseFloat(value, floatValue)) data.moonLocationX = floatValue;
        } else if (_stricmp(key.c_str(), "MoonLocationYEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.moonLocationYEnabled = boolValue;
                moonLocationYEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "MoonLocationY") == 0) {
            if (TryParseFloat(value, floatValue)) data.moonLocationY = floatValue;
        } else if (_stricmp(key.c_str(), "2CEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.exp2CEnabled = boolValue;
                exp2CEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "2C") == 0) {
            if (TryParseFloat(value, floatValue)) data.exp2C = floatValue;
        } else if (_stricmp(key.c_str(), "2DEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.exp2DEnabled = boolValue;
                exp2DEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "2D") == 0) {
            if (TryParseFloat(value, floatValue)) data.exp2D = floatValue;
        } else if (_stricmp(key.c_str(), "NightSkyRotationEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.nightSkyRotationEnabled = boolValue;
                nightSkyRotationEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "NightSkyRotation") == 0) {
            if (TryParseFloat(value, floatValue)) data.nightSkyRotation = floatValue;
        } else if (_stricmp(key.c_str(), "FogEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.fogEnabled = boolValue;
                fogEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "Fog") == 0) {
            if (TryParseFloat(value, floatValue)) data.fogPercent = floatValue;
        } else if (_stricmp(key.c_str(), "PlainFogEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.plainFogEnabled = boolValue;
                plainFogEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "PlainFog") == 0) {
            if (TryParseFloat(value, floatValue)) data.plainFog = floatValue;
        } else if (_stricmp(key.c_str(), "Wind") == 0) {
            if (TryParseFloat(value, floatValue)) data.wind = floatValue;
        } else if (_stricmp(key.c_str(), "NoWind") == 0) {
            if (TryParseBool(value, boolValue)) data.noWind = boolValue;
        } else if (_stricmp(key.c_str(), "PuddleScaleEnabled") == 0) {
            if (TryParseBool(value, boolValue)) {
                data.puddleScaleEnabled = boolValue;
                puddleScaleEnabledSeen = true;
            }
        } else if (_stricmp(key.c_str(), "PuddleScale") == 0) {
            if (TryParseFloat(value, floatValue)) data.puddleScale = floatValue;
        }
    }

    fclose(fp);
    if (!headerSeen) return false;
    data.forceCloudsPercent = 0.0f;
    data.forceCloudsEnabled = false;
    if (!cloudHeightEnabledSeen) data.cloudHeightEnabled = !FloatNearlyEqual(data.cloudHeight, 1.0f);
    if (!cloudDensityEnabledSeen) data.cloudDensityEnabled = !FloatNearlyEqual(data.cloudDensity, 1.0f);
    if (!midCloudsEnabledSeen) data.midCloudsEnabled = !FloatNearlyEqual(data.midClouds, 1.0f);
    if (!highCloudsEnabledSeen) data.highCloudsEnabled = !FloatNearlyEqual(data.highClouds, 1.0f);
    if (!sunLocationXEnabledSeen) data.sunLocationXEnabled = false;
    if (!sunLocationYEnabledSeen) data.sunLocationYEnabled = false;
    if (!moonLocationXEnabledSeen) data.moonLocationXEnabled = false;
    if (!moonLocationYEnabledSeen) data.moonLocationYEnabled = false;
    if (!exp2CEnabledSeen) data.exp2CEnabled = false;
    if (!exp2DEnabledSeen) data.exp2DEnabled = false;
    if (!nightSkyRotationEnabledSeen) data.nightSkyRotationEnabled = false;
    if (!fogEnabledSeen) data.fogEnabled = !FloatNearlyEqual(data.fogPercent, 0.0f);
    if (!plainFogEnabledSeen) data.plainFogEnabled = !FloatNearlyEqual(data.plainFog, 0.0f);
    if (!puddleScaleEnabledSeen) data.puddleScaleEnabled = !FloatNearlyEqual(data.puddleScale, 0.0f);
    data.exp2C = min(15.0f, max(0.0f, data.exp2C));
    data.exp2D = min(15.0f, max(0.0f, data.exp2D));
    data.nightSkyRotation = min(15.0f, max(-15.0f, data.nightSkyRotation));
    data.plainFog = min(15.0f, max(0.0f, data.plainFog));
    data.sunLocationX = min(180.0f, max(-180.0f, data.sunLocationX));
    data.sunLocationY = min(180.0f, max(-180.0f, data.sunLocationY));
    data.moonLocationX = min(180.0f, max(-180.0f, data.moonLocationX));
    data.moonLocationY = min(180.0f, max(-180.0f, data.moonLocationY));
    if (sawLegacyAlias) {
        Log("[preset] loaded legacy cloud aliases from %s\n", path);
    }
    outData = data;
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
