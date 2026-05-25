#include "pch.h"

#include "overlay_internal.h"
#include "community_service.h"
#include "sky_texture_override.h"
#include "preset_service.h"
#include "runtime_shared.h"

#include <imgui.h>
#include <reshade.hpp>

#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace overlay_internal {
char g_timeEditText[32] = "";
int g_timeEditLastMinute = -1;
bool g_timeEditActive = false;
bool g_timeEditFocusRequest = false;
bool g_timeEditHadFocus = false;
int g_realClockDialPendingMinute = -1;
bool g_hideRealGameTimeWarningThisSession = false;
bool g_suppressRealGameTimeWarningChoice = false;
void LogTimeUiAction(
    const char* action,
    bool detachedEdit,
    bool regionScoped,
    bool overrideMaskTime,
    const WeatherPresetData& editData) {
    const PresetScheduleStatus schedule = PresetSchedule_GetStatus();
    Log("[time-ui] action=%s detached=%u regionScoped=%u editRegion=%d maskTime=%u "
        "schedule{enabled=%u active=%u entry=%d preset=%s blending=%u} "
        "runtime{ctrl=%u freeze=%u progress=%u matchGame=%u cadence=%.0f target=%.4f current=%.4f currentValid=%u applyReq=%u} "
        "edit{override=%u progress=%u matchGame=%u cadence=%.0f hour=%.4f}\n",
        action ? action : "unknown",
        detachedEdit ? 1u : 0u,
        regionScoped ? 1u : 0u,
        Preset_GetEditRegion(),
        overrideMaskTime ? 1u : 0u,
        schedule.enabled ? 1u : 0u,
        schedule.active ? 1u : 0u,
        schedule.activeEntryIndex,
        schedule.activePresetFile.empty() ? "<none>" : schedule.activePresetFile.c_str(),
        schedule.blending ? 1u : 0u,
        g_timeCtrlActive.load() ? 1u : 0u,
        g_timeFreeze.load() ? 1u : 0u,
        g_timeProgressVisualTime.load() ? 1u : 0u,
        g_timeProgressMatchGameTime.load() ? 1u : 0u,
        g_timeProgressCadenceMs.load(),
        g_timeTargetHour.load(),
        g_timeCurrentHour.load(),
        g_timeCurrentHourValid.load() ? 1u : 0u,
        g_timeApplyRequest.load() ? 1u : 0u,
        editData.visualTimeOverride ? 1u : 0u,
        editData.progressVisualTime ? 1u : 0u,
        editData.progressVisualTimeMatchGameTime ? 1u : 0u,
        editData.progressVisualTimeIntervalMs,
        editData.timeHour);
}

void DisableScheduleForManualTimeEdit(const char* action) {
    if (!PresetSchedule_IsEnabled()) {
        return;
    }
    Log("[time-ui] disabling time schedule due to manual time edit: %s\n", action ? action : "unknown");
    PresetSchedule_SetEnabled(false);
}

void DrawTimeControls() {
    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData editData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask overrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    bool editChanged = false;
    bool manualTimeEditChanged = false;

    ImGui::Spacing();
    ImGui::SeparatorText("Time");
    bool visualTimeOverride = detachedEdit ? editData.visualTimeOverride : (g_timeCtrlActive.load() && g_timeFreeze.load());
    bool realGameTime = !regionScoped && g_realGameTimeEnabled.load();
    bool progressVisualTime = detachedEdit ? editData.progressVisualTime : g_timeProgressVisualTime.load();
    bool progressVisualTimeMatchGameTime = detachedEdit ? editData.progressVisualTimeMatchGameTime : g_timeProgressMatchGameTime.load();
    float progressVisualTimeIntervalMs = detachedEdit
        ? ClampProgressVisualTimeIntervalMs(editData.progressVisualTimeIntervalMs)
        : ClampProgressVisualTimeIntervalMs(g_timeProgressCadenceMs.load());
    const bool timeEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::TimeControls) &&
                             g_timeLayoutReady.load() &&
                             WeatherTickReady();
    const bool realGameTimeAvailable = !regionScoped && RealGameTimeReady();
    const bool timeValueLocked = regionScoped && !overrideMask.time;
    bool timeOverrideChanged = false;
    if (regionScoped) {
        timeOverrideChanged = DrawOverrideToggle(&overrideMask.time);
        ImGui::SameLine();
    }
    if (timeOverrideChanged) {
        editChanged = true;
    }
    int timeMode = realGameTime ? 1 : (visualTimeOverride ? 2 : 0);
    int nextTimeMode = timeMode;
    if (ImGui::RadioButton("Native", timeMode == 0)) {
        nextTimeMode = 0;
    }
    ImGui::SameLine();
    if (!realGameTimeAvailable) {
        ImGui::BeginDisabled();
    }
    if (ImGui::RadioButton("Real In-Game Time", timeMode == 1)) {
        if (g_hideRealGameTimeWarningThisSession) {
            nextTimeMode = 1;
        } else {
            g_suppressRealGameTimeWarningChoice = false;
            ImGui::OpenPopup("Real In-Game Time Warning");
        }
    }
    if (!realGameTimeAvailable) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (!timeEnabled || timeValueLocked) {
        ImGui::BeginDisabled();
    }
    if (ImGui::RadioButton("Visual Time Override", timeMode == 2)) {
        nextTimeMode = 2;
    }
    if (!timeEnabled || timeValueLocked) {
        ImGui::EndDisabled();
    }
    const bool realTimeScaleActive =
        fabsf(g_realGameTimeDayScale.load() - 1.0f) > 0.001f ||
        fabsf(g_realGameTimeNightScale.load() - 1.0f) > 0.001f;
    if (!regionScoped && !realGameTime && realTimeScaleActive) {
        ImGui::SameLine();
        ImGui::TextDisabled("Time Scale Active");
    }
    if (ImGui::BeginPopupModal("Real In-Game Time Warning", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 38.0f);
        ImGui::TextUnformatted(
            "Real In-Game Time changes the game's actual world clock. "
            "Advancing or speeding up time may affect quests, NPC behavior, "
            "and other timed systems.");
        ImGui::Spacing();
        ImGui::TextUnformatted("Please use this feature carefully and at your own risk.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::Checkbox("Don't display again for this session", &g_suppressRealGameTimeWarningChoice);
        ImGui::Spacing();
        if (ImGui::Button("I understand", ImVec2(140.0f, 0.0f))) {
            g_hideRealGameTimeWarningThisSession = g_suppressRealGameTimeWarningChoice;
            nextTimeMode = 1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    const bool timeModeChanged = nextTimeMode != timeMode;
    if (PresetSchedule_IsEnabled() && timeModeChanged) {
        ImGui::SetTooltip("Changing this will disable Time Schedule.");
    }
    if (timeModeChanged) {
        visualTimeOverride = nextTimeMode == 2;
        realGameTime = nextTimeMode == 1;
        if (realGameTime) {
            g_realGameTimeEnabled.store(true);
            g_realGameTimeSetMinuteRequest.store(-1);
            g_realGameTimeDayDeltaRequest.store(0);
            g_timeCtrlActive.store(false);
            g_timeFreeze.store(false);
            g_timeProgressVisualTime.store(false);
            g_timeProgressMatchGameTime.store(false);
            g_timeProgressLastTick.store(0);
            g_timeProgressMatchLastMinute.store(-1);
            g_timeProgressMatchPendingMs.store(0);
            g_timeApplyRequest.store(true);
        } else if (detachedEdit) {
            if (!regionScoped) {
                g_realGameTimeEnabled.store(false);
                g_realGameTimeSetMinuteRequest.store(-1);
                g_realGameTimeDayDeltaRequest.store(0);
            }
            editData.visualTimeOverride = visualTimeOverride;
            if (!visualTimeOverride) {
                editData.progressVisualTime = false;
                editData.progressVisualTimeMatchGameTime = false;
            }
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_realGameTimeEnabled.store(realGameTime);
            g_realGameTimeSetMinuteRequest.store(-1);
            g_realGameTimeDayDeltaRequest.store(0);
            g_timeCtrlActive.store(visualTimeOverride);
            g_timeFreeze.store(visualTimeOverride);
            if (!visualTimeOverride) {
                g_timeProgressVisualTime.store(false);
                g_timeProgressMatchGameTime.store(false);
                g_timeProgressLastTick.store(0);
                g_timeProgressMatchLastMinute.store(-1);
                g_timeProgressMatchPendingMs.store(0);
            }
            g_timeApplyRequest.store(true);
        }
        LogTimeUiAction("time-mode", detachedEdit, regionScoped, regionScoped ? overrideMask.time : true, editData);
        manualTimeEditChanged = true;
        GUI_SetStatus(visualTimeOverride ? "Visual Time Override enabled" :
            (realGameTime ? "Real In-Game Time controls selected" : "Native time selected"));
    }
    if (!visualTimeOverride) {
        progressVisualTime = false;
        progressVisualTimeMatchGameTime = false;
    }
    float hudGameTimeHour = 0.0f;
    const bool hasHudGameTime = TryGetHudGameTimeHour(&hudGameTimeHour);
    const int nativeHour = g_gameTimeProbeHour.load();
    const int nativeMinute = g_gameTimeProbeMinute.load();
    const bool hasNativeGameTime = nativeHour >= 0 && nativeHour < 24 && nativeMinute >= 0 && nativeMinute < 60;
    const float nativeGameTimeHour = hasNativeGameTime
        ? MinuteOfDayToHour(nativeHour * 60 + nativeMinute)
        : 0.0f;
    bool timeReset = false;
    if (!realGameTime) {
    const bool progressDisabled = !(timeEnabled && visualTimeOverride) || timeValueLocked;
    if (progressDisabled) {
        ImGui::BeginDisabled();
    }
    const bool progressVisualTimeChanged = ImGui::Checkbox("Progress Visual Time", &progressVisualTime);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Advances the visual time override instead of keeping the sky frozen at one hour.");
    }
    if (progressDisabled) {
        ImGui::EndDisabled();
    }
    if (progressVisualTimeChanged) {
        progressVisualTime = visualTimeOverride && progressVisualTime;
        if (detachedEdit) {
            editData.visualTimeOverride = visualTimeOverride;
            editData.progressVisualTime = progressVisualTime;
            if (!progressVisualTime) {
                editData.progressVisualTimeMatchGameTime = false;
            }
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeProgressVisualTime.store(progressVisualTime);
            if (!progressVisualTime) {
                g_timeProgressMatchGameTime.store(false);
                g_timeProgressMatchLastMinute.store(-1);
                g_timeProgressMatchPendingMs.store(0);
            }
            g_timeProgressLastTick.store(progressVisualTime ? GetTickCount64() : 0);
            g_timeCtrlActive.store(visualTimeOverride);
            g_timeFreeze.store(visualTimeOverride);
            g_timeApplyRequest.store(true);
        }
        LogTimeUiAction("progress-toggle", detachedEdit, regionScoped, regionScoped ? overrideMask.time : true, editData);
        manualTimeEditChanged = true;
        GUI_SetStatus(progressVisualTime ? "Progress Visual Time enabled" : "Progress Visual Time disabled");
    }

    char hudGameTimeText[32] = {};
    if (hasHudGameTime) {
        FormatGameClockFromHour(hudGameTimeHour, hudGameTimeText, sizeof(hudGameTimeText));
    }

    if (!progressVisualTime) {
        progressVisualTimeMatchGameTime = false;
    }
    const bool matchClockDisabled = progressDisabled || !progressVisualTime;
    if (matchClockDisabled) {
        ImGui::BeginDisabled();
    }
    const bool matchClockChanged = ImGui::Checkbox("Match In-Game Clock", &progressVisualTimeMatchGameTime);
    if (matchClockDisabled) {
        ImGui::EndDisabled();
    }
    if (hasHudGameTime) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", hudGameTimeText);
    }
    if (matchClockChanged) {
        progressVisualTimeMatchGameTime = progressVisualTime && progressVisualTimeMatchGameTime;
        const int matchedHudMinute = (progressVisualTimeMatchGameTime && hasHudGameTime)
            ? HourToMinuteOfDayFloor(hudGameTimeHour)
            : -1;
        const float matchedHudHour = matchedHudMinute >= 0
            ? MinuteOfDayToHour(matchedHudMinute)
            : 0.0f;
        if (detachedEdit) {
            editData.visualTimeOverride = visualTimeOverride;
            editData.progressVisualTime = progressVisualTime;
            editData.progressVisualTimeMatchGameTime = progressVisualTimeMatchGameTime;
            if (matchedHudMinute >= 0) {
                editData.timeHour = matchedHudHour;
            }
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeProgressMatchGameTime.store(progressVisualTimeMatchGameTime);
            if (matchedHudMinute >= 0) {
                g_timeTargetHour.store(matchedHudHour);
                g_timeProgressMatchLastMinute.store(matchedHudMinute);
            } else {
                g_timeProgressMatchLastMinute.store(-1);
            }
            g_timeProgressMatchPendingMs.store(0);
            g_timeProgressLastTick.store(progressVisualTime ? GetTickCount64() : 0);
            g_timeCtrlActive.store(visualTimeOverride);
            g_timeFreeze.store(visualTimeOverride);
            g_timeApplyRequest.store(true);
        }
        if (progressVisualTimeMatchGameTime) {
            g_timeEditActive = false;
            g_timeEditFocusRequest = false;
            g_timeEditHadFocus = false;
        }
        LogTimeUiAction("progress-match-game-time-toggle", detachedEdit, regionScoped, regionScoped ? overrideMask.time : true, editData);
        manualTimeEditChanged = true;
        GUI_SetStatus(progressVisualTimeMatchGameTime ? "Match In-Game Clock enabled" : "Match In-Game Clock disabled");
    }

    const bool intervalDisabled = progressDisabled || !progressVisualTime;
    if (intervalDisabled) {
        ImGui::BeginDisabled();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Advance Interval");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(progressVisualTimeMatchGameTime
            ? "Controls how often matched in-game clock changes are applied. 0 ms applies every detected HUD clock change."
            : "Controls how often Progress Visual Time steps forward. 0 ms updates every frame.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(125.0f);
    const bool progressIntervalEditing = IsSliderTextEditActive("progress_visual_time_interval");
    bool progressIntervalChanged = false;
    if (progressIntervalEditing) {
        char inputFormat[24] = {};
        if (ConsumeSliderTextEditFocusRequest()) {
            ImGui::SetKeyboardFocusHere();
        }
        progressIntervalChanged = ImGui::InputFloat(
            "##progress_visual_time_interval",
            &progressVisualTimeIntervalMs,
            0.0f,
            0.0f,
            NumericInputFormat("%.0f ms", inputFormat, sizeof(inputFormat)),
            ImGuiInputTextFlags_AutoSelectAll);
        if (progressIntervalChanged) {
            progressVisualTimeIntervalMs = ClampProgressVisualTimeIntervalMs(progressVisualTimeIntervalMs);
        }

        const bool finishEdit = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                                ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                                ImGui::IsItemDeactivated();
        if (finishEdit) {
            EndSliderTextEdit();
        }
    } else {
        progressIntervalChanged = ImGui::SliderFloat("##progress_visual_time_interval", &progressVisualTimeIntervalMs, 0.0f, 5000.0f, "%.0f ms");
        if (!intervalDisabled && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            BeginSliderTextEdit("progress_visual_time_interval");
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Lower values update more smoothly. Higher values step visual time forward in batches.");
    }
    if (intervalDisabled) {
        ImGui::EndDisabled();
    }
    if (progressIntervalChanged) {
        progressVisualTimeIntervalMs = ClampProgressVisualTimeIntervalMs(progressVisualTimeIntervalMs);
        if (detachedEdit) {
            editData.progressVisualTimeIntervalMs = progressVisualTimeIntervalMs;
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeProgressCadenceMs.store(progressVisualTimeIntervalMs);
            if (!g_timeProgressMatchGameTime.load()) {
                g_timeProgressLastTick.store(0);
            }
        }
        LogTimeUiAction("progress-interval", detachedEdit, regionScoped, regionScoped ? overrideMask.time : true, editData);
        manualTimeEditChanged = true;
        GUI_SetStatus("Advance interval changed");
    }
    if (!progressVisualTimeMatchGameTime && ClampProgressVisualTimeIntervalMs(progressVisualTimeIntervalMs) <= 0.5f) {
        ImGui::SameLine();
        ImGui::TextDisabled("Every frame");
    }

    ImGui::SameLine();
    const float resetWidth = 28.0f;
    const float resetX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - resetWidth;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), resetX));
    timeReset = ImGui::Button("R##time_reset", ImVec2(resetWidth, 0.0f));
    if (!timeEnabled) {
        if (!RuntimeFeatureAvailable(RuntimeFeatureId::TimeControls) || !g_timeLayoutReady.load()) {
            DrawFeatureUnavailable(RuntimeFeatureId::TimeControls);
        } else {
            DrawHookUnavailable(RuntimeHookId::WeatherTick);
        }
    }
    } else {
        const SliderRange realTimeScaleRange = ActiveSliderRange(0.01f, 20.0f, 0.01f, 60.0f);
        float dayScale = g_realGameTimeDayScale.load();
        float nightScale = g_realGameTimeNightScale.load();
        bool dayChanged = false;
        bool nightChanged = false;
        const bool resetDay = DrawSliderFloatRow(
            "Day Time Scale", "real_day_scale", &dayScale, realTimeScaleRange.lo, realTimeScaleRange.hi, "x%.2f",
            &dayChanged, nullptr, nullptr, fabsf(dayScale - 1.0f) <= 0.001f, true, 1.0f);
        const bool resetNight = DrawSliderFloatRow(
            "Night Time Scale", "real_night_scale", &nightScale, realTimeScaleRange.lo, realTimeScaleRange.hi, "x%.2f",
            &nightChanged, nullptr, nullptr, fabsf(nightScale - 1.0f) <= 0.001f, true, 1.0f);
        if (resetDay) {
            dayScale = 1.0f;
            dayChanged = true;
        }
        if (resetNight) {
            nightScale = 1.0f;
            nightChanged = true;
        }
        if (dayChanged || nightChanged) {
            dayScale = min(realTimeScaleRange.hi, max(realTimeScaleRange.lo, dayScale));
            nightScale = min(realTimeScaleRange.hi, max(realTimeScaleRange.lo, nightScale));
            g_realGameTimeDayScale.store(dayScale);
            g_realGameTimeNightScale.store(nightScale);
            g_cfg.realGameTimeDayScale = dayScale;
            g_cfg.realGameTimeNightScale = nightScale;
            SaveGeneralConfig();
            GUI_SetStatus("Real game time scale changed");
        }
    }

    const bool runtimeProgressVisualTimeActive =
        g_timeCtrlActive.load() && g_timeFreeze.load() && g_timeProgressVisualTime.load();
    const bool progressUiActive = runtimeProgressVisualTimeActive;
    const bool displayRealTime = realGameTime && hasNativeGameTime;
    const bool displayMatchedHudTime = progressVisualTimeMatchGameTime && hasHudGameTime;
    const float displayedTimeHour = displayRealTime
        ? nativeGameTimeHour
        : displayMatchedHudTime
        ? hudGameTimeHour
        : runtimeProgressVisualTimeActive
        ? g_timeTargetHour.load()
        : (detachedEdit ? editData.timeHour : g_timeTargetHour.load());
    int timeMinutes = displayRealTime
        ? HourToMinuteOfDay(nativeGameTimeHour)
        : displayMatchedHudTime
        ? HourToMinuteOfDay(hudGameTimeHour)
        : progressUiActive
        ? HourToMinuteOfDayFloor(displayedTimeHour)
        : HourToMinuteOfDay(displayedTimeHour);
    if (realGameTime && g_realClockDialPendingMinute >= 0) {
        timeMinutes = g_realClockDialPendingMinute;
    }
    char targetClock[32] = {};
    FormatGameClockFromMinute(timeMinutes, targetClock, sizeof(targetClock));

    const bool manualClockDisabled = realGameTime
        ? !realGameTimeAvailable
        : (!(timeEnabled && visualTimeOverride) || timeValueLocked || progressVisualTimeMatchGameTime);
    if (manualClockDisabled) {
        ImGui::BeginDisabled();
    }
    bool previousDay = false;
    bool nextDay = false;
    bool timeChanged = false;
    bool dialActive = false;
    bool dialReleased = false;
    if (realGameTime) {
        constexpr float kDayButtonWidth = 72.0f;
        constexpr float kDialWidth = 116.0f;
        constexpr float kGap = 8.0f;
        const float rowWidth = kDayButtonWidth * 2.0f + kDialWidth + kGap * 2.0f;
        const float rowY = ImGui::GetCursorPosY();
        const float buttonY = rowY + (kDialWidth - ImGui::GetFrameHeight()) * 0.5f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + max(0.0f, (ImGui::GetContentRegionAvail().x - rowWidth) * 0.5f));
        ImGui::SetCursorPosY(buttonY);
        previousDay = ImGui::Button("-1 Day", ImVec2(kDayButtonWidth, 0.0f));
        ImGui::SameLine(0.0f, kGap);
        ImGui::SetCursorPosY(rowY);
        timeChanged = DrawClockDial("time", &timeMinutes, false);
        dialActive = ImGui::IsItemActive();
        dialReleased = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine(0.0f, kGap);
        ImGui::SetCursorPosY(buttonY);
        nextDay = ImGui::Button("+1 Day", ImVec2(kDayButtonWidth, 0.0f));
    } else {
        timeChanged = DrawClockDial("time", &timeMinutes);
        dialActive = ImGui::IsItemActive();
    }

    if (realGameTime && (previousDay || nextDay)) {
        g_realGameTimeDayDeltaRequest.store(previousDay ? -1 : 1);
        GUI_SetStatus(previousDay ? "Moved real game time back one day" : "Moved real game time forward one day");
    }
    if (realGameTime && timeChanged) {
        g_realClockDialPendingMinute = timeMinutes;
        FormatGameClockFromHour(MinuteOfDayToHour(timeMinutes), targetClock, sizeof(targetClock));
        FormatGameClockFromHour(MinuteOfDayToHour(timeMinutes), g_timeEditText, sizeof(g_timeEditText));
        g_timeEditLastMinute = timeMinutes;
    }
    if (realGameTime && g_realClockDialPendingMinute >= 0 &&
        (dialReleased || (!dialActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left)))) {
        g_realGameTimeSetMinuteRequest.store(g_realClockDialPendingMinute);
        g_realClockDialPendingMinute = -1;
        GUI_SetStatus("Real game time changed");
    } else if (timeChanged && timeEnabled && visualTimeOverride && !progressVisualTimeMatchGameTime) {
        if (detachedEdit) {
            editData.timeHour = MinuteOfDayToHour(timeMinutes);
            editData.visualTimeOverride = true;
            editData.progressVisualTime = progressVisualTime;
            editData.progressVisualTimeMatchGameTime = progressVisualTime && progressVisualTimeMatchGameTime;
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeTargetHour.store(MinuteOfDayToHour(timeMinutes));
            g_timeCtrlActive.store(true);
            g_timeFreeze.store(true);
            g_timeProgressLastTick.store(g_timeProgressVisualTime.load() ? GetTickCount64() : 0);
            g_timeProgressMatchLastMinute.store(-1);
            g_timeProgressMatchPendingMs.store(0);
            g_timeApplyRequest.store(true);
        }
        LogTimeUiAction("clock-dial", detachedEdit, regionScoped, regionScoped ? overrideMask.time : true, editData);
        manualTimeEditChanged = true;
        FormatGameClockFromHour(MinuteOfDayToHour(timeMinutes), targetClock, sizeof(targetClock));
        FormatGameClockFromHour(MinuteOfDayToHour(timeMinutes), g_timeEditText, sizeof(g_timeEditText));
        g_timeEditLastMinute = timeMinutes;
    } else if (g_timeEditLastMinute != timeMinutes && !g_timeEditActive && !dialActive && g_realClockDialPendingMinute < 0) {
        strcpy_s(g_timeEditText, targetClock);
        g_timeEditLastMinute = timeMinutes;
    }

    const float textWidth = 96.0f;
    const float textX = ImGui::GetCursorPosX() + max(0.0f, (ImGui::GetContentRegionAvail().x - textWidth) * 0.5f);
    ImGui::SetCursorPosX(textX);
    bool textSubmitted = false;
    bool textCancelled = false;
    if (g_timeEditActive) {
        ImGui::SetNextItemWidth(textWidth);
        if (g_timeEditFocusRequest) {
            ImGui::SetKeyboardFocusHere();
            g_timeEditFocusRequest = false;
        }
        textSubmitted = ImGui::InputTextWithHint(
            "##time_text",
            "4:20 AM",
            g_timeEditText,
            IM_ARRAYSIZE(g_timeEditText),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        const bool inputActive = ImGui::IsItemActive();
        if (inputActive) {
            g_timeEditHadFocus = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            textCancelled = true;
        } else if (g_timeEditHadFocus && ImGui::IsItemDeactivated() && !textSubmitted) {
            textCancelled = true;
        }
    } else {
        const ImVec2 labelSize = ImGui::CalcTextSize(targetClock);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + max(0.0f, (textWidth - labelSize.x) * 0.5f));
        ImGui::TextUnformatted(targetClock);
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            strcpy_s(g_timeEditText, targetClock);
            g_timeEditActive = true;
            g_timeEditFocusRequest = true;
            g_timeEditHadFocus = false;
        }
    }

    int typedMinutes = timeMinutes;
    bool typedValid = false;
    if (textSubmitted) {
        typedValid = TryParseClockText(g_timeEditText, &typedMinutes);
        if (typedValid) {
            timeMinutes = typedMinutes;
            FormatGameClockFromHour(MinuteOfDayToHour(timeMinutes), g_timeEditText, sizeof(g_timeEditText));
            g_timeEditLastMinute = timeMinutes;
            LogTimeUiAction("time-text-submit", detachedEdit, regionScoped, regionScoped ? overrideMask.time : true, editData);
        } else {
            GUI_SetStatus("Invalid time");
            LogTimeUiAction("time-text-invalid", detachedEdit, regionScoped, regionScoped ? overrideMask.time : true, editData);
        }
        g_timeEditActive = false;
        g_timeEditFocusRequest = false;
        g_timeEditHadFocus = false;
    } else if (textCancelled) {
        g_timeEditActive = false;
        g_timeEditFocusRequest = false;
        g_timeEditHadFocus = false;
        strcpy_s(g_timeEditText, targetClock);
    }

    if (timeReset) {
        const float sourceHour = hasHudGameTime
            ? hudGameTimeHour
            : g_timeCurrentHour.load();
        const int sourceMinute = HourToMinuteOfDay(sourceHour);
        if (detachedEdit) {
            editData.timeHour = MinuteOfDayToHour(sourceMinute);
            editData.visualTimeOverride = true;
            editData.progressVisualTime = progressVisualTime;
            editData.progressVisualTimeMatchGameTime = progressVisualTime && progressVisualTimeMatchGameTime;
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeTargetHour.store(MinuteOfDayToHour(sourceMinute));
            g_timeCtrlActive.store(true);
            g_timeFreeze.store(true);
            g_timeProgressLastTick.store(g_timeProgressVisualTime.load() ? GetTickCount64() : 0);
            g_timeProgressMatchLastMinute.store(-1);
            g_timeProgressMatchPendingMs.store(0);
            g_timeApplyRequest.store(true);
        }
        LogTimeUiAction("time-reset", detachedEdit, regionScoped, regionScoped ? overrideMask.time : true, editData);
        manualTimeEditChanged = true;
        g_timeEditActive = false;
        g_timeEditFocusRequest = false;
        g_timeEditHadFocus = false;
        g_timeEditLastMinute = -1;
        GUI_SetStatus("Time reset");
    } else if (realGameTime && typedValid) {
        g_realGameTimeSetMinuteRequest.store(timeMinutes);
        g_realClockDialPendingMinute = -1;
        GUI_SetStatus("Real game time changed");
    } else if (timeEnabled && visualTimeOverride && !progressVisualTimeMatchGameTime && typedValid) {
        if (detachedEdit) {
            editData.timeHour = MinuteOfDayToHour(timeMinutes);
            editData.visualTimeOverride = true;
            editData.progressVisualTime = progressVisualTime;
            editData.progressVisualTimeMatchGameTime = progressVisualTime && progressVisualTimeMatchGameTime;
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeTargetHour.store(MinuteOfDayToHour(timeMinutes));
            g_timeCtrlActive.store(true);
            g_timeFreeze.store(true);
            g_timeProgressLastTick.store(g_timeProgressVisualTime.load() ? GetTickCount64() : 0);
            g_timeProgressMatchLastMinute.store(-1);
            g_timeProgressMatchPendingMs.store(0);
            g_timeApplyRequest.store(true);
        }
        LogTimeUiAction("time-text-apply", detachedEdit, regionScoped, regionScoped ? overrideMask.time : true, editData);
        manualTimeEditChanged = true;
    }
    if (manualClockDisabled) {
        ImGui::EndDisabled();
    }

    if (manualTimeEditChanged) {
        DisableScheduleForManualTimeEdit("general-time-control");
    }
    if (detachedEdit && editChanged) {
        if (regionScoped) {
            Preset_SetEditRegionDataWithOverrides(editData, overrideMask);
        } else {
            Preset_SetEditRegionData(editData);
        }
    }
}

void DrawWindControls() {
    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData editData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask overrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    bool editChanged = false;

    ImGui::Spacing();
    ImGui::SeparatorText("Wind");
    bool noWind = detachedEdit ? editData.noWind : g_noWind.load();
    const bool windEnabled = !noWind && RuntimeFeatureAvailable(RuntimeFeatureId::WindControls) && WindPackReady();
    DrawSliderById("wind_general", &editData, &overrideMask, &editChanged);
    if (!windEnabled) {
        if (!noWind) {
            if (!RuntimeFeatureAvailable(RuntimeFeatureId::WindControls)) {
                DrawFeatureUnavailable(RuntimeFeatureId::WindControls);
            } else {
                DrawHookUnavailable(RuntimeHookId::WindPack);
            }
        }
    }

    DrawControlById("no_wind", &editData, &overrideMask, &editChanged);

    if (detachedEdit && editChanged) {
        if (regionScoped) {
            Preset_SetEditRegionDataWithOverrides(editData, overrideMask);
        } else {
            Preset_SetEditRegionData(editData);
        }
    }
}

void DrawGeneralControls() {
    if (DrawDisabledTabBody()) {
        return;
    }
    DrawTimeControls();
    DrawWindControls();
    DrawRenoDxInteractionControls();
}

void DrawGeneralTab() {
    DrawGeneralControls();
}

} // namespace overlay_internal
