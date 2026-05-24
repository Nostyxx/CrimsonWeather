#include "pch.h"
#include "runtime_shared.h"
#include "preset_model.h"
#include "preset_schedule.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace preset_internal {

constexpr const char* kTimeScheduleConfigSection = "TimeSchedule";
constexpr int kTimeScheduleDefaultBlendSeconds = 120;
constexpr int kTimeScheduleSourceVisualTimeOverride = 0;
constexpr int kTimeScheduleSourceGameTime = 1;
constexpr bool kPresetVerboseTestLog = false;

const PresetScheduleHost& Host() {
    return GetPresetScheduleHost();
}

bool TryGetTimeScheduleClockHour(float& outHour);

bool g_timeScheduleLoaded = false;
bool g_timeScheduleEnabled = false;
int g_timeScheduleTimeSource = kTimeScheduleSourceVisualTimeOverride;
std::vector<PresetScheduleEntry> g_timeScheduleEntries;
std::string g_timeScheduleActivePresetFile;
int g_timeScheduleActiveEntryIndex = -1;
int g_timeScheduleLastAppliedEntryIndex = -1;
std::string g_timeScheduleLastMissingPresetFile;
bool g_timeScheduleBlendActive = false;
WeatherPresetData g_timeScheduleBlendFrom{};
WeatherPresetPackage g_timeScheduleBlendTargetPackage{};
std::string g_timeScheduleBlendPresetFile;
std::string g_timeScheduleBlendFromPresetFile;
ULONGLONG g_timeScheduleBlendStartTick = 0;
int g_timeScheduleBlendSeconds = 0;
int g_timeScheduleBlendLogBucket = -1;
bool g_timeScheduleUserEditPinned = false;
int g_timeScheduleUserEditPinnedEntryIndex = -1;
std::string g_timeScheduleUserEditPinnedPresetFile;
bool g_timeScheduleSelfVisualGuard = false;
float g_timeScheduleSelfVisualGuardHour = -1.0f;
std::string g_timeScheduleSelfVisualGuardPresetFile;
bool g_timeScheduleSelfVisualGuardSkipLogged = false;
int g_timeScheduleLastDiagMinute = -1;
int g_timeScheduleLastDiagEntryIndex = -9999;
bool g_timeScheduleLastDiagPinned = false;

int NormalizeScheduleMinute(int minute) {
    minute %= 24 * 60;
    if (minute < 0) {
        minute += 24 * 60;
    }
    return minute;
}

PresetScheduleEntry SanitizeScheduleEntry(PresetScheduleEntry entry) {
    entry.startMinute = NormalizeScheduleMinute(entry.startMinute);
    entry.endMinute = NormalizeScheduleMinute(entry.endMinute);
    entry.presetFile = EnsureIniExtension(TrimCopy(entry.presetFile));
    entry.blendSeconds = max(0, entry.blendSeconds);
    return entry;
}

bool IsScheduleEntryUsable(const PresetScheduleEntry& entry) {
    return entry.startMinute != entry.endMinute && !TrimCopy(entry.presetFile).empty();
}

struct ScheduleSegment {
    int start = 0;
    int end = 0;
};

std::vector<ScheduleSegment> ExpandScheduleRange(int startMinute, int endMinute) {
    startMinute = NormalizeScheduleMinute(startMinute);
    endMinute = NormalizeScheduleMinute(endMinute);
    if (startMinute == endMinute) {
        return {};
    }
    if (startMinute < endMinute) {
        return { { startMinute, endMinute } };
    }
    return { { startMinute, 24 * 60 }, { 0, endMinute } };
}

std::vector<ScheduleSegment> SubtractScheduleSegment(ScheduleSegment source, ScheduleSegment cut) {
    if (cut.end <= source.start || cut.start >= source.end) {
        return { source };
    }

    std::vector<ScheduleSegment> out;
    if (cut.start > source.start) {
        out.push_back({ source.start, min(cut.start, source.end) });
    }
    if (cut.end < source.end) {
        out.push_back({ max(cut.end, source.start), source.end });
    }
    return out;
}

std::vector<ScheduleSegment> SubtractScheduleSegments(std::vector<ScheduleSegment> source, const std::vector<ScheduleSegment>& cuts) {
    for (const ScheduleSegment& cut : cuts) {
        std::vector<ScheduleSegment> next;
        for (const ScheduleSegment& segment : source) {
            std::vector<ScheduleSegment> pieces = SubtractScheduleSegment(segment, cut);
            next.insert(next.end(), pieces.begin(), pieces.end());
        }
        source.swap(next);
        if (source.empty()) {
            break;
        }
    }
    return source;
}

void SortScheduleEntries(std::vector<PresetScheduleEntry>& entries) {
    std::sort(entries.begin(), entries.end(), [](const PresetScheduleEntry& a, const PresetScheduleEntry& b) {
        if (a.startMinute != b.startMinute) return a.startMinute < b.startMinute;
        return a.endMinute < b.endMinute;
    });
}

int NormalizeScheduleTimeSource(int source) {
    return source == kTimeScheduleSourceGameTime
        ? kTimeScheduleSourceGameTime
        : kTimeScheduleSourceVisualTimeOverride;
}

int ParseScheduleTimeSource(const char* raw) {
    const std::string value = TrimCopy(raw ? raw : "");
    if (value.empty()) {
        return kTimeScheduleSourceVisualTimeOverride;
    }

    std::string normalized;
    normalized.reserve(value.size());
    for (char c : value) {
        if (c == '-' || c == '_' || std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }

    if (normalized == "1" || normalized == "gametime" || normalized == "ingametime" || normalized == "native" || normalized == "nativetime") {
        return kTimeScheduleSourceGameTime;
    }
    return kTimeScheduleSourceVisualTimeOverride;
}

const char* ScheduleTimeSourceConfigValue(int source) {
    return NormalizeScheduleTimeSource(source) == kTimeScheduleSourceGameTime
        ? "GameTime"
        : "VisualTimeOverride";
}

void SaveTimeScheduleConfig() {
    char iniPath[MAX_PATH] = {};
    BuildIniPath(iniPath, sizeof(iniPath));

    const int oldCount = GetPrivateProfileIntA(kTimeScheduleConfigSection, "EntryCount", 0, iniPath);
    WritePrivateProfileStringA(kTimeScheduleConfigSection, "Enabled", g_timeScheduleEnabled ? "1" : "0", iniPath);
    WritePrivateProfileStringA(kTimeScheduleConfigSection, "TimeSource", ScheduleTimeSourceConfigValue(g_timeScheduleTimeSource), iniPath);

    char value[64] = {};
    sprintf_s(value, "%d", static_cast<int>(g_timeScheduleEntries.size()));
    WritePrivateProfileStringA(kTimeScheduleConfigSection, "EntryCount", value, iniPath);

    for (int i = 0; i < static_cast<int>(g_timeScheduleEntries.size()); ++i) {
        const PresetScheduleEntry& entry = g_timeScheduleEntries[i];
        char key[64] = {};
        sprintf_s(key, "Entry%dStartMinute", i);
        sprintf_s(value, "%d", entry.startMinute);
        WritePrivateProfileStringA(kTimeScheduleConfigSection, key, value, iniPath);

        sprintf_s(key, "Entry%dEndMinute", i);
        sprintf_s(value, "%d", entry.endMinute);
        WritePrivateProfileStringA(kTimeScheduleConfigSection, key, value, iniPath);

        sprintf_s(key, "Entry%dPreset", i);
        WritePrivateProfileStringA(kTimeScheduleConfigSection, key, entry.presetFile.c_str(), iniPath);

        sprintf_s(key, "Entry%dBlendSeconds", i);
        sprintf_s(value, "%d", entry.blendSeconds);
        WritePrivateProfileStringA(kTimeScheduleConfigSection, key, value, iniPath);
    }

    for (int i = static_cast<int>(g_timeScheduleEntries.size()); i < oldCount; ++i) {
        char key[64] = {};
        sprintf_s(key, "Entry%dStartMinute", i);
        WritePrivateProfileStringA(kTimeScheduleConfigSection, key, nullptr, iniPath);
        sprintf_s(key, "Entry%dEndMinute", i);
        WritePrivateProfileStringA(kTimeScheduleConfigSection, key, nullptr, iniPath);
        sprintf_s(key, "Entry%dPreset", i);
        WritePrivateProfileStringA(kTimeScheduleConfigSection, key, nullptr, iniPath);
        sprintf_s(key, "Entry%dBlendSeconds", i);
        WritePrivateProfileStringA(kTimeScheduleConfigSection, key, nullptr, iniPath);
    }
}

void EnsureTimeScheduleLoaded() {
    if (g_timeScheduleLoaded) {
        return;
    }
    g_timeScheduleLoaded = true;

    char iniPath[MAX_PATH] = {};
    BuildIniPath(iniPath, sizeof(iniPath));
    g_timeScheduleEnabled = GetPrivateProfileIntA(kTimeScheduleConfigSection, "Enabled", 0, iniPath) != 0;
    char sourceText[64] = {};
    GetPrivateProfileStringA(kTimeScheduleConfigSection, "TimeSource", "VisualTimeOverride", sourceText, static_cast<DWORD>(sizeof(sourceText)), iniPath);
    g_timeScheduleTimeSource = ParseScheduleTimeSource(sourceText);

    int count = static_cast<int>(GetPrivateProfileIntA(kTimeScheduleConfigSection, "EntryCount", 0, iniPath));
    count = max(0, min(64, count));
    g_timeScheduleEntries.clear();
    for (int i = 0; i < count; ++i) {
        char key[64] = {};
        char preset[MAX_PATH] = {};
        sprintf_s(key, "Entry%dPreset", i);
        GetPrivateProfileStringA(kTimeScheduleConfigSection, key, "", preset, static_cast<DWORD>(sizeof(preset)), iniPath);

        sprintf_s(key, "Entry%dStartMinute", i);
        const int startMinute = GetPrivateProfileIntA(kTimeScheduleConfigSection, key, -1, iniPath);
        sprintf_s(key, "Entry%dEndMinute", i);
        const int endMinute = GetPrivateProfileIntA(kTimeScheduleConfigSection, key, -1, iniPath);
        sprintf_s(key, "Entry%dBlendSeconds", i);
        const int blendSeconds = GetPrivateProfileIntA(kTimeScheduleConfigSection, key, kTimeScheduleDefaultBlendSeconds, iniPath);

        PresetScheduleEntry entry{};
        entry.startMinute = startMinute;
        entry.endMinute = endMinute;
        entry.presetFile = preset;
        entry.blendSeconds = blendSeconds;
        entry = SanitizeScheduleEntry(entry);
        if (startMinute >= 0 && endMinute >= 0 && IsScheduleEntryUsable(entry)) {
            g_timeScheduleEntries.push_back(entry);
        }
    }

    SortScheduleEntries(g_timeScheduleEntries);
    SaveTimeScheduleConfig();
    Log("[preset-schedule] loaded enabled=%d source=%s entries=%d\n",
        g_timeScheduleEnabled ? 1 : 0,
        ScheduleTimeSourceConfigValue(g_timeScheduleTimeSource),
        static_cast<int>(g_timeScheduleEntries.size()));
}

bool ReplaceTimeScheduleEntryWithNew(int editedIndex, PresetScheduleEntry newEntry) {
    EnsureTimeScheduleLoaded();
    newEntry = SanitizeScheduleEntry(newEntry);
    if (!IsScheduleEntryUsable(newEntry) || Host().findPresetIndexByFileName(newEntry.presetFile) < 0) {
        return false;
    }

    std::vector<PresetScheduleEntry> next;
    const std::vector<ScheduleSegment> newSegments = ExpandScheduleRange(newEntry.startMinute, newEntry.endMinute);
    for (int i = 0; i < static_cast<int>(g_timeScheduleEntries.size()); ++i) {
        if (i == editedIndex) {
            continue;
        }

        const PresetScheduleEntry existing = g_timeScheduleEntries[i];
        std::vector<ScheduleSegment> remaining = SubtractScheduleSegments(
            ExpandScheduleRange(existing.startMinute, existing.endMinute),
            newSegments);
        for (const ScheduleSegment& segment : remaining) {
            if (segment.start == segment.end) {
                continue;
            }
            PresetScheduleEntry trimmed = existing;
            trimmed.startMinute = NormalizeScheduleMinute(segment.start);
            trimmed.endMinute = NormalizeScheduleMinute(segment.end);
            if (IsScheduleEntryUsable(trimmed)) {
                next.push_back(trimmed);
            }
        }
    }

    next.push_back(newEntry);
    SortScheduleEntries(next);
    g_timeScheduleEntries.swap(next);
    SaveTimeScheduleConfig();
    TimeScheduleClearUserEditPin();
    TimeScheduleClearSelfVisualGuard();
    g_timeScheduleBlendActive = false;
    g_timeScheduleActivePresetFile.clear();
    g_timeScheduleActiveEntryIndex = -1;
    g_timeScheduleLastAppliedEntryIndex = -1;
    g_timeScheduleLastMissingPresetFile.clear();
    return true;
}

int ActiveScheduleEntryIndexForMinute(int minuteOfDay) {
    minuteOfDay = NormalizeScheduleMinute(minuteOfDay);
    for (int i = 0; i < static_cast<int>(g_timeScheduleEntries.size()); ++i) {
        const PresetScheduleEntry& entry = g_timeScheduleEntries[i];
        const int start = NormalizeScheduleMinute(entry.startMinute);
        const int end = NormalizeScheduleMinute(entry.endMinute);
        if (start == end) {
            continue;
        }
        if (start < end) {
            if (minuteOfDay >= start && minuteOfDay < end) {
                return i;
            }
        } else if (minuteOfDay >= start || minuteOfDay < end) {
            return i;
        }
    }
    return -1;
}

void TimeScheduleClearUserEditPin() {
    g_timeScheduleUserEditPinned = false;
    g_timeScheduleUserEditPinnedEntryIndex = -1;
    g_timeScheduleUserEditPinnedPresetFile.clear();
}

void TimeScheduleClearSelfVisualGuard() {
    g_timeScheduleSelfVisualGuard = false;
    g_timeScheduleSelfVisualGuardHour = -1.0f;
    g_timeScheduleSelfVisualGuardPresetFile.clear();
    g_timeScheduleSelfVisualGuardSkipLogged = false;
}

float ScheduleHourDelta(float a, float b) {
    float delta = std::fabs(NormalizeHour24(a) - NormalizeHour24(b));
    if (delta > 12.0f) {
        delta = 24.0f - delta;
    }
    return delta;
}

void TimeScheduleMarkSelfVisualGuard(const WeatherPresetData& data, const char* presetFile) {
    if (!data.visualTimeOverride) {
        TimeScheduleClearSelfVisualGuard();
        return;
    }

    g_timeScheduleSelfVisualGuard = true;
    g_timeScheduleSelfVisualGuardHour = NormalizeHour24(data.timeHour);
    g_timeScheduleSelfVisualGuardPresetFile = presetFile ? presetFile : "";
    g_timeScheduleSelfVisualGuardSkipLogged = false;
    Log("[preset-schedule-diag] reason=self-visual-guard preset=%s hour=%.4f\n",
        g_timeScheduleSelfVisualGuardPresetFile.empty() ? "<none>" : g_timeScheduleSelfVisualGuardPresetFile.c_str(),
        g_timeScheduleSelfVisualGuardHour);
}

void LogTimeScheduleDecision(const char* reason, int minuteOfDay, int entryIndex) {
    const char* entryPreset = "<gap>";
    if (entryIndex >= 0 && entryIndex < static_cast<int>(g_timeScheduleEntries.size())) {
        entryPreset = g_timeScheduleEntries[entryIndex].presetFile.c_str();
    }
    const char* selectedPreset = Host().hasSelection()
        ? Host().presetItemAt(Host().selectedPresetIndex()).fileName.c_str()
        : "<none>";
    float scheduleClockHour = 0.0f;
    const bool scheduleClockValid = TryGetTimeScheduleClockHour(scheduleClockHour);
    Log("[preset-schedule-diag] reason=%s minute=%d entry=%d entryPreset=%s "
        "activeEntry=%d lastApplied=%d activeFile=%s selected=%s blend=%u pinned=%u pinnedEntry=%d pinnedPreset=%s "
        "selfGuard=%u selfHour=%.4f editDraft=%u lastRegion=%d time{source=%s ctrl=%u freeze=%u progress=%u target=%.4f current=%.4f hud=%02d:%02d schedule=%.4f valid=%u}\n",
        reason ? reason : "unknown",
        minuteOfDay,
        entryIndex,
        entryPreset,
        g_timeScheduleActiveEntryIndex,
        g_timeScheduleLastAppliedEntryIndex,
        g_timeScheduleActivePresetFile.empty() ? "<none>" : g_timeScheduleActivePresetFile.c_str(),
        selectedPreset,
        g_timeScheduleBlendActive ? 1u : 0u,
        g_timeScheduleUserEditPinned ? 1u : 0u,
        g_timeScheduleUserEditPinnedEntryIndex,
        g_timeScheduleUserEditPinnedPresetFile.empty() ? "<none>" : g_timeScheduleUserEditPinnedPresetFile.c_str(),
        g_timeScheduleSelfVisualGuard ? 1u : 0u,
        g_timeScheduleSelfVisualGuardHour,
        Host().editDraftValid() ? 1u : 0u,
        Host().lastAppliedRegion(),
        ScheduleTimeSourceConfigValue(g_timeScheduleTimeSource),
        g_timeCtrlActive.load() ? 1u : 0u,
        g_timeFreeze.load() ? 1u : 0u,
        g_timeProgressVisualTime.load() ? 1u : 0u,
        g_timeTargetHour.load(),
        g_timeCurrentHour.load(),
        g_timeUiClockHour24.load(),
        g_timeUiClockMinute.load(),
        scheduleClockHour,
        scheduleClockValid ? 1u : 0u);
}

bool TryGetTimeScheduleClockHour(float& outHour) {
    if (g_timeScheduleTimeSource == kTimeScheduleSourceGameTime) {
        if (!g_timeUiClockSourceValid.load() || !g_timeUiClockValid.load()) {
            return false;
        }

        const int hour = g_timeUiClockHour24.load();
        const int minute = g_timeUiClockMinute.load();
        if (hour < 0 || hour >= 24 || minute < 0 || minute >= 60) {
            return false;
        }

        outHour = NormalizeHour24(static_cast<float>(hour) + static_cast<float>(minute) / 60.0f);
        return true;
    }

    if (!g_timeCurrentHourValid.load()) {
        return false;
    }

    if (g_timeCtrlActive.load() && g_timeFreeze.load()) {
        outHour = g_timeTargetHour.load();
    } else {
        outHour = g_timeCurrentHour.load();
    }
    outHour = NormalizeHour24(outHour);
    return true;
}

void ApplyScheduledPresetInstantOrStartBlend(int index) {
    if (index < 0 || index >= static_cast<int>(g_timeScheduleEntries.size())) {
        return;
    }

    const PresetScheduleEntry& entry = g_timeScheduleEntries[index];
    const int presetIndex = Host().findPresetIndexByFileName(entry.presetFile);
    if (presetIndex < 0) {
        g_timeScheduleActiveEntryIndex = index;
        if (!EqualsNoCase(g_timeScheduleLastMissingPresetFile, entry.presetFile)) {
            g_timeScheduleLastMissingPresetFile = entry.presetFile;
            Log("[W] scheduled preset missing: %s\n", entry.presetFile.c_str());
            GUI_SetStatus(("Scheduled preset missing: " + entry.presetFile).c_str());
        }
        return;
    }

    if (EqualsNoCase(g_timeScheduleActivePresetFile, Host().presetItemAt(presetIndex).fileName)) {
        g_timeScheduleActiveEntryIndex = index;
        g_timeScheduleLastAppliedEntryIndex = index;
        return;
    }

    WeatherPresetPackage package{};
    if (!Host().loadPresetForRuntime(presetIndex, package)) {
        Log("[W] failed to load scheduled preset: %s\n", Host().presetItemAt(presetIndex).fileName.c_str());
        GUI_SetStatus("Scheduled preset load failed");
        return;
    }

    g_timeScheduleLastMissingPresetFile.clear();

    if (entry.blendSeconds <= 0) {
        g_timeScheduleBlendActive = false;
        LogTimeScheduleDecision("apply-instant", -1, index);
        const WeatherPresetData appliedData = EffectivePresetDataForRegion(package, Host().currentRegionId());
        if (Host().selectPresetIndex(presetIndex, true, "Scheduled preset applied: ", nullptr, "scheduled")) {
            g_timeScheduleActivePresetFile = Host().presetItemAt(presetIndex).fileName;
            g_timeScheduleActiveEntryIndex = index;
            g_timeScheduleLastAppliedEntryIndex = index;
            TimeScheduleMarkSelfVisualGuard(appliedData, g_timeScheduleActivePresetFile.c_str());
        }
        return;
    }

    std::string blendFromPresetFile = g_timeScheduleActivePresetFile;
    if (blendFromPresetFile.empty() && Host().hasSelection()) {
        blendFromPresetFile = Host().presetItemAt(Host().selectedPresetIndex()).fileName;
    }
    if (EqualsNoCase(blendFromPresetFile, Host().presetItemAt(presetIndex).fileName)) {
        g_timeScheduleBlendActive = false;
        LogTimeScheduleDecision("same-preset-instant", -1, index);
        const WeatherPresetData appliedData = EffectivePresetDataForRegion(package, Host().currentRegionId());
        if (Host().selectPresetIndex(presetIndex, true, "Scheduled preset applied: ", nullptr, "scheduled")) {
            g_timeScheduleActivePresetFile = Host().presetItemAt(presetIndex).fileName;
            g_timeScheduleActiveEntryIndex = index;
            g_timeScheduleLastAppliedEntryIndex = index;
            TimeScheduleMarkSelfVisualGuard(appliedData, g_timeScheduleActivePresetFile.c_str());
        }
        return;
    }

    // Capture the live evaluated state first. This includes active region overrides
    // and any in-progress blend that already touched the sliders this frame.
    const WeatherPresetData blendFrom = Host().captureCurrentData();
    LogTimeScheduleDecision("start-blend", -1, index);
    if (!Host().selectPresetIndex(presetIndex, false, "Scheduled preset blending: ", nullptr, "scheduled")) {
        return;
    }

    g_timeScheduleBlendFrom = blendFrom;
    g_timeScheduleBlendTargetPackage = package;
    g_timeScheduleBlendPresetFile = Host().presetItemAt(presetIndex).fileName;
    g_timeScheduleBlendFromPresetFile = blendFromPresetFile;
    g_timeScheduleBlendStartTick = GetTickCount64();
    g_timeScheduleBlendSeconds = entry.blendSeconds;
    g_timeScheduleBlendLogBucket = -1;
    g_timeScheduleBlendActive = true;
    g_timeScheduleActivePresetFile = Host().presetItemAt(presetIndex).fileName;
    g_timeScheduleActiveEntryIndex = index;
    g_timeScheduleLastAppliedEntryIndex = index;

    Log("[preset-schedule] blending to %s over %ds\n",
        g_timeScheduleBlendPresetFile.c_str(),
        g_timeScheduleBlendSeconds);
}

void TickScheduledPresetBlend() {
    if (!g_timeScheduleBlendActive) {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    const float durationMs = static_cast<float>(max(1, g_timeScheduleBlendSeconds)) * 1000.0f;
    const float progress = ClampPresetFloat(static_cast<float>(now - g_timeScheduleBlendStartTick) / durationMs, 0.0f, 1.0f);
    const int regionId = Host().currentRegionId();
    const WeatherPresetData target = EffectivePresetDataForRegion(g_timeScheduleBlendTargetPackage, regionId);
    const WeatherPresetData blended = BlendPresetData(g_timeScheduleBlendFrom, target, progress);
    Host().applyScheduledData(blended, regionId);

    const int progressBucket = min(4, max(0, static_cast<int>(progress * 4.0f)));
    if (kPresetVerboseTestLog && progressBucket != g_timeScheduleBlendLogBucket) {
        g_timeScheduleBlendLogBucket = progressBucket;
        Log("[preset-schedule-test] blend-progress preset=%s progress=%d%%\n",
            g_timeScheduleBlendPresetFile.c_str(),
            progressBucket * 25);
    }

    if (progress >= 0.999f) {
        Host().applyScheduledData(target, regionId);
        g_timeScheduleBlendActive = false;
        TimeScheduleMarkSelfVisualGuard(target, g_timeScheduleBlendPresetFile.c_str());
        Log("[preset-schedule] blend finished: %s\n", g_timeScheduleBlendPresetFile.c_str());
        GUI_SetStatus(("Scheduled preset active: " + GetPresetDisplayNameFromFileName(g_timeScheduleBlendPresetFile)).c_str());
    }
}

bool TimeScheduleRuntimeTick(bool worldReady) {
    EnsureTimeScheduleLoaded();
    if (g_timeScheduleEnabled || g_timeScheduleBlendActive) {
        Host().ensureInitialized();
    }

    if (g_timeScheduleBlendActive) {
        TickScheduledPresetBlend();
        if (g_timeScheduleBlendActive) {
            return true;
        }
    }

    float scheduleClockHour = 0.0f;
    const bool scheduleClockValid = TryGetTimeScheduleClockHour(scheduleClockHour);
    if (!g_timeScheduleEnabled || !worldReady || !scheduleClockValid) {
        return g_timeScheduleBlendActive;
    }

    scheduleClockHour = NormalizeHour24(scheduleClockHour);
    if (g_timeScheduleSelfVisualGuard && g_timeScheduleTimeSource == kTimeScheduleSourceVisualTimeOverride) {
        if (!(g_timeCtrlActive.load() && g_timeFreeze.load())) {
            TimeScheduleClearSelfVisualGuard();
        } else if (ScheduleHourDelta(scheduleClockHour, g_timeScheduleSelfVisualGuardHour) <= (1.0f / 60.0f)) {
            if (!g_timeScheduleSelfVisualGuardSkipLogged) {
                LogTimeScheduleDecision("self-visual-guard-skip", -1, g_timeScheduleActiveEntryIndex);
                g_timeScheduleSelfVisualGuardSkipLogged = true;
            }
            return g_timeScheduleBlendActive;
        } else {
            LogTimeScheduleDecision("self-visual-guard-cleared", -1, g_timeScheduleActiveEntryIndex);
            TimeScheduleClearSelfVisualGuard();
        }
    } else if (g_timeScheduleSelfVisualGuard) {
        TimeScheduleClearSelfVisualGuard();
    }

    const int minuteOfDay = NormalizeScheduleMinute(static_cast<int>(std::floor(scheduleClockHour * 60.0f + 0.0001f)));
    const int entryIndex = ActiveScheduleEntryIndexForMinute(minuteOfDay);
    const bool diagChanged =
        minuteOfDay != g_timeScheduleLastDiagMinute ||
        entryIndex != g_timeScheduleLastDiagEntryIndex ||
        g_timeScheduleUserEditPinned != g_timeScheduleLastDiagPinned;
    if (diagChanged) {
        LogTimeScheduleDecision("tick", minuteOfDay, entryIndex);
        g_timeScheduleLastDiagMinute = minuteOfDay;
        g_timeScheduleLastDiagEntryIndex = entryIndex;
        g_timeScheduleLastDiagPinned = g_timeScheduleUserEditPinned;
    }
    if (entryIndex < 0) {
        g_timeScheduleActiveEntryIndex = -1;
        TimeScheduleClearUserEditPin();
        return g_timeScheduleBlendActive;
    }

    if (g_timeScheduleUserEditPinned) {
        const PresetScheduleEntry& entry = g_timeScheduleEntries[entryIndex];
        if (EqualsNoCase(entry.presetFile, g_timeScheduleUserEditPinnedPresetFile)) {
            g_timeScheduleActiveEntryIndex = entryIndex;
            g_timeScheduleLastAppliedEntryIndex = entryIndex;
            g_timeScheduleActivePresetFile = entry.presetFile;
            if (diagChanged) {
                LogTimeScheduleDecision("pinned-user-edit-skip", minuteOfDay, entryIndex);
            }
            return g_timeScheduleBlendActive;
        }
        LogTimeScheduleDecision("clear-user-edit-pin-new-entry", minuteOfDay, entryIndex);
        TimeScheduleClearUserEditPin();
    }

    ApplyScheduledPresetInstantOrStartBlend(entryIndex);
    return g_timeScheduleBlendActive;
}

void TimeScheduleDisableForManualSelection() {
    EnsureTimeScheduleLoaded();
    if (!g_timeScheduleEnabled && !g_timeScheduleBlendActive) {
        return;
    }
    g_timeScheduleEnabled = false;
    g_timeScheduleBlendActive = false;
    g_timeScheduleActivePresetFile.clear();
    g_timeScheduleActiveEntryIndex = -1;
    g_timeScheduleLastAppliedEntryIndex = -1;
    g_timeScheduleBlendPresetFile.clear();
    g_timeScheduleBlendFromPresetFile.clear();
    TimeScheduleClearUserEditPin();
    TimeScheduleClearSelfVisualGuard();
    SaveTimeScheduleConfig();
    Log("[preset-schedule] disabled by manual preset selection\n");
}

void TimeScheduleCancelBlendForUserEdit() {
    if (!g_timeScheduleBlendActive) {
        return;
    }
    g_timeScheduleBlendActive = false;
    g_timeScheduleBlendPresetFile.clear();
    g_timeScheduleBlendFromPresetFile.clear();
    g_timeScheduleBlendLogBucket = -1;
    Log("[preset-schedule] blend cancelled by preset edit\n");
}

void TimeSchedulePinCurrentEntryForUserEdit() {
    EnsureTimeScheduleLoaded();
    TimeScheduleCancelBlendForUserEdit();
    TimeScheduleClearSelfVisualGuard();
    if (!g_timeScheduleEnabled ||
        g_timeScheduleActiveEntryIndex < 0 ||
        g_timeScheduleActiveEntryIndex >= static_cast<int>(g_timeScheduleEntries.size())) {
        return;
    }

    const PresetScheduleEntry& entry = g_timeScheduleEntries[g_timeScheduleActiveEntryIndex];
    g_timeScheduleUserEditPinned = true;
    g_timeScheduleUserEditPinnedEntryIndex = g_timeScheduleActiveEntryIndex;
    g_timeScheduleUserEditPinnedPresetFile = entry.presetFile;
    g_timeScheduleActivePresetFile = entry.presetFile;
    g_timeScheduleLastAppliedEntryIndex = g_timeScheduleActiveEntryIndex;
    LogTimeScheduleDecision("pin-user-edit", -1, g_timeScheduleActiveEntryIndex);
    Log("[preset-schedule] pinned current entry for user edit: %s\n", entry.presetFile.c_str());
}

bool ScheduleIsEnabled() {
    EnsureTimeScheduleLoaded();
    return g_timeScheduleEnabled;
}

void ScheduleSetEnabled(bool enabled) {
    EnsureTimeScheduleLoaded();
    if (g_timeScheduleEnabled == enabled) {
        return;
    }
    g_timeScheduleEnabled = enabled;
    if (!enabled) {
        g_timeScheduleBlendActive = false;
        g_timeScheduleActivePresetFile.clear();
    }
    TimeScheduleClearUserEditPin();
    TimeScheduleClearSelfVisualGuard();
    SaveTimeScheduleConfig();
    GUI_SetStatus(enabled ? "Time schedule enabled" : "Time schedule disabled");
    Log("[preset-schedule] %s\n", enabled ? "enabled" : "disabled");
}

int ScheduleGetTimeSource() {
    EnsureTimeScheduleLoaded();
    return NormalizeScheduleTimeSource(g_timeScheduleTimeSource);
}

void ScheduleSetTimeSource(int source) {
    EnsureTimeScheduleLoaded();
    source = NormalizeScheduleTimeSource(source);
    if (g_timeScheduleTimeSource == source) {
        return;
    }

    g_timeScheduleTimeSource = source;
    g_timeScheduleBlendActive = false;
    g_timeScheduleActivePresetFile.clear();
    g_timeScheduleActiveEntryIndex = -1;
    g_timeScheduleLastAppliedEntryIndex = -1;
    g_timeScheduleLastDiagMinute = -1;
    g_timeScheduleLastDiagEntryIndex = -9999;
    TimeScheduleClearUserEditPin();
    TimeScheduleClearSelfVisualGuard();
    SaveTimeScheduleConfig();
    GUI_SetStatus(source == kTimeScheduleSourceGameTime
        ? "Time schedule source: in-game time"
        : "Time schedule source: visual time override");
    Log("[preset-schedule] time source=%s\n", ScheduleTimeSourceConfigValue(source));
}

std::vector<PresetScheduleRow> ScheduleBuildRows() {
    EnsureTimeScheduleLoaded();

    std::vector<PresetScheduleRow> rows;
    std::vector<ScheduleSegment> explicitSegments;
    for (int i = 0; i < static_cast<int>(g_timeScheduleEntries.size()); ++i) {
        const PresetScheduleEntry& entry = g_timeScheduleEntries[i];
        const int presetIndex = Host().findPresetIndexByFileName(entry.presetFile);

        PresetScheduleRow row{};
        row.gap = false;
        row.startMinute = entry.startMinute;
        row.endMinute = entry.endMinute;
        row.entryIndex = i;
        row.presetFile = entry.presetFile;
        row.displayName = presetIndex >= 0 ? Host().presetItemAt(presetIndex).displayName : GetPresetDisplayNameFromFileName(entry.presetFile);
        row.presetMissing = presetIndex < 0;
        row.blendSeconds = entry.blendSeconds;
        rows.push_back(row);

        std::vector<ScheduleSegment> segments = ExpandScheduleRange(entry.startMinute, entry.endMinute);
        explicitSegments.insert(explicitSegments.end(), segments.begin(), segments.end());
    }

    std::sort(explicitSegments.begin(), explicitSegments.end(), [](const ScheduleSegment& a, const ScheduleSegment& b) {
        if (a.start != b.start) return a.start < b.start;
        return a.end < b.end;
    });

    std::vector<ScheduleSegment> merged;
    for (const ScheduleSegment& segment : explicitSegments) {
        if (segment.start == segment.end) {
            continue;
        }
        if (!merged.empty() && segment.start <= merged.back().end) {
            merged.back().end = max(merged.back().end, segment.end);
        } else {
            merged.push_back(segment);
        }
    }

    if (merged.empty()) {
        rows.push_back({ true, 0, 0, -1, "", "gap - no preset", false, 0 });
    } else {
        for (size_t i = 0; i < merged.size(); ++i) {
            const int gapStart = merged[i].end % (24 * 60);
            const int gapEnd = merged[(i + 1) % merged.size()].start;
            const bool lastWrapGap = i + 1 == merged.size();
            if (!lastWrapGap && gapStart < gapEnd) {
                rows.push_back({ true, gapStart, gapEnd, -1, "", "gap - no preset", false, 0 });
            } else if (lastWrapGap && gapStart != gapEnd) {
                rows.push_back({ true, gapStart, gapEnd, -1, "", "gap - no preset", false, 0 });
            }
        }
    }

    std::sort(rows.begin(), rows.end(), [](const PresetScheduleRow& a, const PresetScheduleRow& b) {
        if (a.startMinute != b.startMinute) return a.startMinute < b.startMinute;
        if (a.gap != b.gap) return !a.gap;
        return a.endMinute < b.endMinute;
    });

    return rows;
}

PresetScheduleStatus ScheduleGetStatus() {
    EnsureTimeScheduleLoaded();
    PresetScheduleStatus status{};
    status.enabled = g_timeScheduleEnabled;
    status.timeSource = NormalizeScheduleTimeSource(g_timeScheduleTimeSource);
    float scheduleClockHour = 0.0f;
    status.timeSourceValid = TryGetTimeScheduleClockHour(scheduleClockHour);
    status.currentMinute = status.timeSourceValid
        ? NormalizeScheduleMinute(static_cast<int>(std::floor(NormalizeHour24(scheduleClockHour) * 60.0f + 0.0001f)))
        : -1;
    status.activeEntryIndex = g_timeScheduleActiveEntryIndex;
    status.blending = g_timeScheduleBlendActive;

    if (g_timeScheduleActiveEntryIndex >= 0 &&
        g_timeScheduleActiveEntryIndex < static_cast<int>(g_timeScheduleEntries.size())) {
        const PresetScheduleEntry& entry = g_timeScheduleEntries[g_timeScheduleActiveEntryIndex];
        status.active = true;
        status.activePresetFile = entry.presetFile;
        const int presetIndex = Host().findPresetIndexByFileName(entry.presetFile);
        status.activeDisplayName = presetIndex >= 0
            ? Host().presetItemAt(presetIndex).displayName
            : GetPresetDisplayNameFromFileName(entry.presetFile);
    }

    if (g_timeScheduleBlendActive) {
        status.blendToDisplayName = GetPresetDisplayNameFromFileName(g_timeScheduleBlendPresetFile);
        status.blendFromDisplayName = g_timeScheduleBlendFromPresetFile.empty()
            ? "Current"
            : GetPresetDisplayNameFromFileName(g_timeScheduleBlendFromPresetFile);
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG elapsedMs = now - g_timeScheduleBlendStartTick;
        const int totalMs = max(1, g_timeScheduleBlendSeconds) * 1000;
        const ULONGLONG cappedElapsedMs = elapsedMs > static_cast<ULONGLONG>(totalMs)
            ? static_cast<ULONGLONG>(totalMs)
            : elapsedMs;
        const int remainingMs = max(0, totalMs - static_cast<int>(cappedElapsedMs));
        status.blendRemainingSeconds = (remainingMs + 999) / 1000;
    }

    return status;
}

bool ScheduleAddEntry(const PresetScheduleEntry& entry) {
    return ReplaceTimeScheduleEntryWithNew(-1, entry);
}

bool ScheduleUpdateEntry(int index, const PresetScheduleEntry& entry) {
    EnsureTimeScheduleLoaded();
    if (index < 0 || index >= static_cast<int>(g_timeScheduleEntries.size())) {
        return false;
    }
    return ReplaceTimeScheduleEntryWithNew(index, entry);
}

bool ScheduleDeleteEntry(int index) {
    EnsureTimeScheduleLoaded();
    if (index < 0 || index >= static_cast<int>(g_timeScheduleEntries.size())) {
        return false;
    }
    g_timeScheduleEntries.erase(g_timeScheduleEntries.begin() + index);
    g_timeScheduleBlendActive = false;
    g_timeScheduleActiveEntryIndex = -1;
    g_timeScheduleLastAppliedEntryIndex = -1;
    g_timeScheduleActivePresetFile.clear();
    TimeScheduleClearUserEditPin();
    TimeScheduleClearSelfVisualGuard();
    SaveTimeScheduleConfig();
    GUI_SetStatus("Time schedule entry deleted");
    Log("[preset-schedule] deleted entry %d\n", index);
    return true;
}

bool ScheduleParseAmPm(const char* text, int& outMinute) {
    outMinute = 0;
    std::string value = TrimCopy(text ? text : "");
    if (value.empty()) {
        return false;
    }

    std::string upper;
    upper.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            continue;
        }
        if (c == '.') {
            upper.push_back(':');
        } else {
            upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
    }

    bool hasMarker = false;
    bool pm = false;
    if (upper.size() >= 2 && upper.compare(upper.size() - 2, 2, "AM") == 0) {
        hasMarker = true;
        pm = false;
        upper.resize(upper.size() - 2);
    } else if (upper.size() >= 2 && upper.compare(upper.size() - 2, 2, "PM") == 0) {
        hasMarker = true;
        pm = true;
        upper.resize(upper.size() - 2);
    }

    if (upper.empty()) {
        return false;
    }

    int hour = 0;
    int minute = 0;
    const size_t colon = upper.find(':');
    if (colon == std::string::npos) {
        for (char c : upper) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        if (!hasMarker && upper.size() == 4) {
            hour = atoi(upper.substr(0, 2).c_str());
            minute = atoi(upper.substr(2, 2).c_str());
        } else if (!hasMarker && upper.size() == 3) {
            hour = atoi(upper.substr(0, 1).c_str());
            minute = atoi(upper.substr(1, 2).c_str());
        } else {
            hour = atoi(upper.c_str());
        }
    } else {
        const std::string hourText = upper.substr(0, colon);
        const std::string minuteText = upper.substr(colon + 1);
        if (hourText.empty() || minuteText.empty() || minuteText.size() > 2) {
            return false;
        }
        for (char c : hourText) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        for (char c : minuteText) {
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        }
        hour = atoi(hourText.c_str());
        minute = atoi(minuteText.c_str());
    }

    if (minute < 0 || minute > 59) {
        return false;
    }

    if (hasMarker) {
        if (hour < 1 || hour > 12) {
            return false;
        }
        int hour24 = hour % 12;
        if (pm) {
            hour24 += 12;
        }
        outMinute = hour24 * 60 + minute;
        return true;
    }

    if (hour < 0 || hour > 23) {
        return false;
    }
    outMinute = hour * 60 + minute;
    return true;
}

std::string ScheduleFormatAmPm(int minute) {
    minute = NormalizeScheduleMinute(minute);
    const int hour24 = minute / 60;
    const int minutePart = minute % 60;
    int hour12 = hour24 % 12;
    if (hour12 == 0) {
        hour12 = 12;
    }
    char out[32] = {};
    sprintf_s(out, "%d:%02d %s", hour12, minutePart, hour24 >= 12 ? "PM" : "AM");
    return out;
}

int ScheduleDefaultBlendSeconds() {
    return kTimeScheduleDefaultBlendSeconds;
}


bool ScheduleNeedsWorldTick() {
    EnsureTimeScheduleLoaded();
    return g_timeScheduleEnabled || g_timeScheduleBlendActive;
}

} // namespace preset_internal
