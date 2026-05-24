#pragma once

#include "preset_model.h"

#include <string>
#include <vector>

namespace preset_internal {

struct PresetScheduleHost {
    int (*findPresetIndexByFileName)(const std::string& fileName);
    bool (*hasSelection)();
    int (*selectedPresetIndex)();
    int (*presetCount)();
    const PresetListItem& (*presetItemAt)(int index);
    void (*ensureInitialized)();
    bool (*loadPresetForRuntime)(int index, WeatherPresetPackage& outPackage);
    bool (*selectPresetIndex)(
        int index,
        bool applyImmediately,
        const char* statusPrefix,
        const char* toastPrefix,
        const char* logVerb);
    WeatherPresetData (*captureCurrentData)();
    int (*currentRegionId)();
    void (*applyScheduledData)(const WeatherPresetData& data, int regionId);
    bool (*editDraftValid)();
    int (*lastAppliedRegion)();
};

const PresetScheduleHost& GetPresetScheduleHost();

void EnsureTimeScheduleLoaded();
bool TimeScheduleRuntimeTick(bool worldReady);
void TimeScheduleDisableForManualSelection();
void TimeScheduleCancelBlendForUserEdit();
void TimeSchedulePinCurrentEntryForUserEdit();
void TimeScheduleClearUserEditPin();
void TimeScheduleClearSelfVisualGuard();
bool ScheduleNeedsWorldTick();

bool ScheduleIsEnabled();
void ScheduleSetEnabled(bool enabled);
int ScheduleGetTimeSource();
void ScheduleSetTimeSource(int source);
std::vector<PresetScheduleRow> ScheduleBuildRows();
PresetScheduleStatus ScheduleGetStatus();
bool ScheduleAddEntry(const PresetScheduleEntry& entry);
bool ScheduleUpdateEntry(int index, const PresetScheduleEntry& entry);
bool ScheduleDeleteEntry(int index);
bool ScheduleParseAmPm(const char* text, int& outMinute);
std::string ScheduleFormatAmPm(int minute);
int ScheduleDefaultBlendSeconds();

} // namespace preset_internal
