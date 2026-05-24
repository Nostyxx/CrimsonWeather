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
    bool progressVisualTime = detachedEdit ? editData.progressVisualTime : g_timeProgressVisualTime.load();
    bool progressVisualTimeMatchGameTime = detachedEdit ? editData.progressVisualTimeMatchGameTime : g_timeProgressMatchGameTime.load();
    float progressVisualTimeIntervalMs = detachedEdit
        ? ClampProgressVisualTimeIntervalMs(editData.progressVisualTimeIntervalMs)
        : ClampProgressVisualTimeIntervalMs(g_timeProgressCadenceMs.load());
    const bool timeEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::TimeControls) &&
                             g_timeLayoutReady.load() &&
                             WeatherTickReady();
    const bool timeValueLocked = regionScoped && !overrideMask.time;
    if (!timeEnabled) {
        ImGui::BeginDisabled();
    }
    bool timeOverrideChanged = false;
    const bool visualTimeChanged = regionScoped
        ? DrawOverrideCheckboxRow("Visual Time Override", "visual_time", &visualTimeOverride, &overrideMask.time, &timeOverrideChanged)
        : ImGui::Checkbox("Visual Time Override", &visualTimeOverride);
    if (PresetSchedule_IsEnabled() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Changing this will disable Time Schedule.");
    }
    if (timeOverrideChanged) {
        editChanged = true;
    }
    if (visualTimeChanged) {
        if (detachedEdit) {
            editData.visualTimeOverride = visualTimeOverride;
            if (!visualTimeOverride) {
                editData.progressVisualTime = false;
                editData.progressVisualTimeMatchGameTime = false;
            }
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
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
        LogTimeUiAction("visual-toggle", detachedEdit, regionScoped, regionScoped ? overrideMask.time : true, editData);
        manualTimeEditChanged = true;
        GUI_SetStatus(visualTimeOverride ? "Visual time override enabled" : "Visual time override disabled");
    }
    if (!visualTimeOverride) {
        progressVisualTime = false;
        progressVisualTimeMatchGameTime = false;
    }
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

    float hudGameTimeHour = 0.0f;
    const bool hasHudGameTime = TryGetHudGameTimeHour(&hudGameTimeHour);
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
    const bool timeReset = ImGui::Button("R##time_reset", ImVec2(resetWidth, 0.0f));
    if (!timeEnabled) {
        ImGui::EndDisabled();
        if (!RuntimeFeatureAvailable(RuntimeFeatureId::TimeControls) || !g_timeLayoutReady.load()) {
            DrawFeatureUnavailable(RuntimeFeatureId::TimeControls);
        } else {
            DrawHookUnavailable(RuntimeHookId::WeatherTick);
        }
    }

    const bool runtimeProgressVisualTimeActive =
        g_timeCtrlActive.load() && g_timeFreeze.load() && g_timeProgressVisualTime.load();
    const bool progressUiActive = runtimeProgressVisualTimeActive;
    const bool displayMatchedHudTime = progressVisualTimeMatchGameTime && hasHudGameTime;
    const float displayedTimeHour = displayMatchedHudTime
        ? hudGameTimeHour
        : runtimeProgressVisualTimeActive
        ? g_timeTargetHour.load()
        : (detachedEdit ? editData.timeHour : g_timeTargetHour.load());
    int timeMinutes = displayMatchedHudTime
        ? HourToMinuteOfDay(hudGameTimeHour)
        : progressUiActive
        ? HourToMinuteOfDayFloor(displayedTimeHour)
        : HourToMinuteOfDay(displayedTimeHour);
    char targetClock[32] = {};
    FormatGameClockFromMinute(timeMinutes, targetClock, sizeof(targetClock));

    const bool manualClockDisabled = !(timeEnabled && visualTimeOverride) || timeValueLocked || progressVisualTimeMatchGameTime;
    if (manualClockDisabled) {
        ImGui::BeginDisabled();
    }
    const bool timeChanged = DrawClockDial("time", &timeMinutes);
    const bool dialActive = ImGui::IsItemActive();

    if (timeChanged && timeEnabled && visualTimeOverride && !progressVisualTimeMatchGameTime) {
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
    } else if (g_timeEditLastMinute != timeMinutes && !g_timeEditActive && !dialActive) {
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
}

void DrawGeneralTab() {
    DrawGeneralControls();
}

} // namespace overlay_internal
