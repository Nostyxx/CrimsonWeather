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
char g_newPresetName[128] = "NewPreset.ini";
char g_presetFilter[96] = "";
char g_scheduleStartText[32] = "6:00 PM";
char g_scheduleEndText[32] = "9:00 PM";
int g_schedulePresetIndex = -1;
int g_scheduleBlendMinutes = 2;
int g_scheduleBlendSeconds = 0;
int g_scheduleEditingIndex = -1;
bool g_schedulePopupOpenRequest = false;
int FindPresetUiIndexByFileName(const std::string& fileName) {
    for (int i = 0; i < Preset_GetCount(); ++i) {
        if (_stricmp(Preset_GetFileName(i), fileName.c_str()) == 0) {
            return i;
        }
    }
    return -1;
}

void OpenTimeScheduleEntryPopup(const PresetScheduleRow* row) {
    if (row) {
        strcpy_s(g_scheduleStartText, PresetSchedule_FormatAmPm(row->startMinute).c_str());
        strcpy_s(g_scheduleEndText, PresetSchedule_FormatAmPm(row->endMinute).c_str());
        g_schedulePresetIndex = row->gap ? max(0, Preset_GetSelectedIndex()) : FindPresetUiIndexByFileName(row->presetFile);
        g_scheduleBlendMinutes = row->gap ? (PresetSchedule_DefaultBlendSeconds() / 60) : max(0, row->blendSeconds / 60);
        g_scheduleBlendSeconds = row->gap ? (PresetSchedule_DefaultBlendSeconds() % 60) : max(0, row->blendSeconds % 60);
        g_scheduleEditingIndex = row->gap ? -1 : row->entryIndex;
    } else {
        strcpy_s(g_scheduleStartText, "6:00 PM");
        strcpy_s(g_scheduleEndText, "9:00 PM");
        g_schedulePresetIndex = max(0, Preset_GetSelectedIndex());
        if (Preset_GetCount() <= 0) {
            g_schedulePresetIndex = -1;
        }
        g_scheduleBlendMinutes = PresetSchedule_DefaultBlendSeconds() / 60;
        g_scheduleBlendSeconds = PresetSchedule_DefaultBlendSeconds() % 60;
        g_scheduleEditingIndex = -1;
    }
    g_schedulePopupOpenRequest = true;
}

std::string FormatScheduleBlend(int seconds) {
    seconds = max(0, seconds);
    char buf[32] = {};
    sprintf_s(buf, "%dm %ds", seconds / 60, seconds % 60);
    return buf;
}

struct ScheduleTimelineSegment {
    int startMinute = 0;
    int endMinute = 0;
    int rowIndex = -1;
};

void AddScheduleTimelineSegmentsForRow(const PresetScheduleRow& row, int rowIndex, std::vector<ScheduleTimelineSegment>& out) {
    int start = row.startMinute % (24 * 60);
    int end = row.endMinute % (24 * 60);
    if (start < 0) start += 24 * 60;
    if (end < 0) end += 24 * 60;

    if (start == end) {
        if (row.gap) {
            out.push_back({ 0, 24 * 60, rowIndex });
        }
        return;
    }

    if (start < end) {
        out.push_back({ start, end, rowIndex });
    } else {
        out.push_back({ start, 24 * 60, rowIndex });
        out.push_back({ 0, end, rowIndex });
    }
}

ImU32 ScheduleTimelineColor(const PresetScheduleRow& row) {
    if (row.gap) {
        return ImGui::GetColorU32(ImVec4(0.76f, 0.74f, 0.66f, 1.0f));
    }
    if (row.presetMissing) {
        return ImGui::GetColorU32(ImVec4(0.56f, 0.20f, 0.20f, 1.0f));
    }

    static const ImVec4 kPalette[] = {
        ImVec4(0.00f, 0.45f, 0.70f, 1.0f), // Okabe-Ito blue
        ImVec4(0.90f, 0.62f, 0.00f, 1.0f), // orange
        ImVec4(0.00f, 0.62f, 0.45f, 1.0f), // bluish green
        ImVec4(0.80f, 0.47f, 0.65f, 1.0f), // reddish purple
        ImVec4(0.84f, 0.37f, 0.00f, 1.0f), // vermillion
        ImVec4(0.34f, 0.71f, 0.91f, 1.0f), // sky blue
        ImVec4(0.94f, 0.89f, 0.26f, 1.0f), // yellow
        ImVec4(0.56f, 0.56f, 0.56f, 1.0f), // neutral gray
    };

    const std::string key = row.presetFile.empty() ? row.displayName : row.presetFile;
    unsigned int hash = 2166136261u;
    for (char c : key) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return ImGui::GetColorU32(kPalette[hash % (sizeof(kPalette) / sizeof(kPalette[0]))]);
}

int FindScheduleTimelineRowAtMinute(const std::vector<ScheduleTimelineSegment>& segments, int minuteOfDay) {
    minuteOfDay %= 24 * 60;
    if (minuteOfDay < 0) {
        minuteOfDay += 24 * 60;
    }

    for (const ScheduleTimelineSegment& segment : segments) {
        if (minuteOfDay >= segment.startMinute && minuteOfDay < segment.endMinute) {
            return segment.rowIndex;
        }
    }
    if (minuteOfDay == 24 * 60 - 1 && !segments.empty()) {
        return segments.back().rowIndex;
    }
    return -1;
}

void DrawTimeScheduleTimeline(const std::vector<PresetScheduleRow>& rows, const PresetScheduleStatus& status) {
    if (rows.empty()) {
        return;
    }

    std::vector<ScheduleTimelineSegment> segments;
    segments.reserve(rows.size() + 2);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        AddScheduleTimelineSegmentsForRow(rows[i], i, segments);
    }

    std::sort(segments.begin(), segments.end(), [](const ScheduleTimelineSegment& a, const ScheduleTimelineSegment& b) {
        if (a.startMinute != b.startMinute) return a.startMinute < b.startMinute;
        return a.endMinute < b.endMinute;
    });

    ImGui::Spacing();
    const float width = max(300.0f, ImGui::GetContentRegionAvail().x);
    const float height = 88.0f;
    ImGui::InvisibleButton("##time_schedule_timeline", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = hovered && ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool scheduleEnabled = status.enabled;
    const ImVec2 boxMin = ImGui::GetItemRectMin();
    const ImVec2 boxMax = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImU32 panelBg = ImGui::GetColorU32(scheduleEnabled ? ImVec4(0.13f, 0.13f, 0.12f, 1.0f) : ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
    const ImU32 panelBorder = ImGui::GetColorU32(scheduleEnabled ? ImVec4(0.34f, 0.34f, 0.31f, 1.0f) : ImVec4(0.23f, 0.23f, 0.22f, 1.0f));
    const ImU32 trackBg = ImGui::GetColorU32(scheduleEnabled ? ImVec4(0.08f, 0.08f, 0.08f, 1.0f) : ImVec4(0.12f, 0.12f, 0.12f, 1.0f));
    const ImU32 tickColor = ImGui::GetColorU32(scheduleEnabled ? ImVec4(0.52f, 0.52f, 0.48f, 1.0f) : ImVec4(0.36f, 0.36f, 0.34f, 1.0f));
    const ImU32 gapText = ImGui::GetColorU32(scheduleEnabled ? ImVec4(0.20f, 0.20f, 0.18f, 1.0f) : ImVec4(0.46f, 0.46f, 0.43f, 1.0f));
    const ImU32 labelText = ImGui::GetColorU32(scheduleEnabled ? ImVec4(0.95f, 0.96f, 0.98f, 1.0f) : ImVec4(0.58f, 0.58f, 0.55f, 1.0f));
    const ImU32 mutedText = ImGui::GetColorU32(scheduleEnabled ? ImVec4(0.62f, 0.62f, 0.58f, 1.0f) : ImVec4(0.44f, 0.44f, 0.42f, 1.0f));
    const ImU32 footerText = ImGui::GetColorU32(scheduleEnabled ? ImVec4(0.78f, 0.80f, 0.78f, 1.0f) : ImVec4(0.50f, 0.50f, 0.47f, 1.0f));
    const ImU32 currentLine = ImGui::GetColorU32(ImVec4(1.0f, 0.18f, 0.16f, 1.0f));
    const ImU32 disabledPresetFill = ImGui::GetColorU32(ImVec4(0.28f, 0.28f, 0.27f, 1.0f));
    const ImU32 disabledGapFill = ImGui::GetColorU32(ImVec4(0.20f, 0.20f, 0.19f, 1.0f));

    draw->AddRectFilled(boxMin, boxMax, panelBg, 7.0f);
    draw->AddRect(boxMin, boxMax, panelBorder, 7.0f);

    const float marginX = 14.0f;
    const float trackY = boxMin.y + 14.0f;
    const float trackH = 28.0f;
    const float trackX0 = boxMin.x + marginX;
    const float trackX1 = boxMax.x - marginX;
    const float trackW = max(1.0f, trackX1 - trackX0);
    draw->AddRectFilled(ImVec2(trackX0, trackY), ImVec2(trackX1, trackY + trackH), trackBg, 6.0f);

    auto minuteToX = [&](int minute) {
        return trackX0 + (static_cast<float>(minute) / static_cast<float>(24 * 60)) * trackW;
    };

    for (const ScheduleTimelineSegment& segment : segments) {
        if (segment.rowIndex < 0 || segment.rowIndex >= static_cast<int>(rows.size())) {
            continue;
        }
        const PresetScheduleRow& row = rows[segment.rowIndex];
        const float x0 = minuteToX(segment.startMinute);
        const float x1 = minuteToX(segment.endMinute);
        if (x1 <= x0 + 1.0f) {
            continue;
        }

        const bool activeSegment = scheduleEnabled && !row.gap && row.entryIndex == status.activeEntryIndex;
        const ImU32 segmentColor = scheduleEnabled
            ? ScheduleTimelineColor(row)
            : (row.gap ? disabledGapFill : disabledPresetFill);
        draw->AddRectFilled(ImVec2(x0, trackY), ImVec2(x1, trackY + trackH), segmentColor);
        if (activeSegment) {
            draw->AddRect(ImVec2(x0 + 1.0f, trackY + 1.0f), ImVec2(x1 - 1.0f, trackY + trackH - 1.0f),
                ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.85f)), 0.0f, 0, 2.0f);
        }
        const std::string label = row.gap ? "gap" : (row.presetMissing ? "missing" : row.displayName);
        const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
        if ((x1 - x0) > textSize.x + 12.0f) {
            const ImU32 textColor = row.gap ? gapText : labelText;
            draw->AddText(ImVec2(x0 + ((x1 - x0) - textSize.x) * 0.5f, trackY + 7.0f), textColor, label.c_str());
        }
    }

    for (int hour = 0; hour <= 24; hour += 6) {
        const float x = minuteToX(hour * 60);
        draw->AddLine(ImVec2(x, trackY + trackH), ImVec2(x, trackY + trackH + 5.0f), tickColor, 1.0f);
        const std::string tick = PresetSchedule_FormatAmPm(hour * 60);
        const ImVec2 tickSize = ImGui::CalcTextSize(tick.c_str());
        float tickX = x - tickSize.x * 0.5f;
        if (hour == 0) tickX = x;
        if (hour == 24) tickX = x - tickSize.x;
        draw->AddText(ImVec2(tickX, trackY + trackH + 7.0f), mutedText, tick.c_str());
    }

    int currentMinute = status.currentMinute;
    int currentRowIndex = -1;
    if (status.timeSourceValid && currentMinute >= 0) {
        if (scheduleEnabled) {
            currentRowIndex = FindScheduleTimelineRowAtMinute(segments, currentMinute);
            const float x = minuteToX(currentMinute);
            draw->AddLine(ImVec2(x, trackY - 2.0f), ImVec2(x, trackY + trackH + 6.0f), currentLine, 1.5f);
        }
    }

    std::string currentPreset = Preset_HasSelection() ? Preset_GetSelectedDisplayName() : "None";
    if (!scheduleEnabled) {
        currentPreset = "Schedule Off";
    } else if (status.blending) {
        currentPreset = status.blendFromDisplayName + " -> " + status.blendToDisplayName + " (" + FormatScheduleBlend(status.blendRemainingSeconds) + ")";
    } else if (status.active) {
        currentPreset = status.activeDisplayName;
    } else if (currentRowIndex >= 0 && currentRowIndex < static_cast<int>(rows.size())) {
        const PresetScheduleRow& currentRow = rows[currentRowIndex];
        if (!currentRow.gap) {
            currentPreset = currentRow.presetMissing ? (currentRow.displayName + " (missing)") : currentRow.displayName;
        }
    }
    const std::string currentTime = currentMinute >= 0 ? PresetSchedule_FormatAmPm(currentMinute) : "--";
    const std::string footer = "Current Time = " + currentTime + " | Current Preset: " + currentPreset;
    const ImVec2 footerSize = ImGui::CalcTextSize(footer.c_str());
    draw->AddText(ImVec2(trackX0 + max(0.0f, (trackW - footerSize.x) * 0.5f), boxMax.y - 18.0f), footerText, footer.c_str());

    if (hovered) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float clampedX = ClampSliderValue(mouse.x, { trackX0, trackX1 });
        int minute = static_cast<int>(((clampedX - trackX0) / trackW) * static_cast<float>(24 * 60));
        minute = min(24 * 60 - 1, max(0, minute));
        const int rowIndex = FindScheduleTimelineRowAtMinute(segments, minute);
        if (rowIndex >= 0 && rowIndex < static_cast<int>(rows.size())) {
            const PresetScheduleRow& row = rows[rowIndex];
            const std::string range = PresetSchedule_FormatAmPm(row.startMinute) + " -> " + PresetSchedule_FormatAmPm(row.endMinute);
            if (row.gap) {
                ImGui::SetTooltip("%s\nGap - click to set preset", range.c_str());
            } else {
                ImGui::SetTooltip("%s\n%s%s\nClick to edit",
                    range.c_str(),
                    row.displayName.c_str(),
                    row.presetMissing ? " (missing)" : "");
            }
            if (clicked && (!row.gap || (row.startMinute != row.endMinute && Preset_GetCount() > 0))) {
                OpenTimeScheduleEntryPopup(&row);
            }
        }
    }
}

void ShiftScheduleTimeText(char* text, size_t textSize, int deltaMinutes) {
    int minute = 0;
    if (!PresetSchedule_ParseAmPm(text, minute)) {
        return;
    }
    minute = (minute + deltaMinutes) % (24 * 60);
    if (minute < 0) {
        minute += 24 * 60;
    }
    strcpy_s(text, textSize, PresetSchedule_FormatAmPm(minute).c_str());
}

void DrawScheduleTimeInput(const char* label, char* text, size_t textSize) {
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(128.0f);
    ImGui::InputText(label, text, textSize);
    ImGui::SameLine();
    if (ImGui::Button("-1h")) {
        ShiftScheduleTimeText(text, textSize, -60);
    }
    ImGui::SameLine();
    if (ImGui::Button("+1h")) {
        ShiftScheduleTimeText(text, textSize, 60);
    }
    ImGui::PopID();
}

void DrawTimeScheduleEntryPopup() {
    if (!ImGui::BeginPopupModal("Time Schedule Entry", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextUnformatted(g_scheduleEditingIndex >= 0 ? "Edit schedule entry" : "New schedule entry");
    ImGui::Separator();

    DrawScheduleTimeInput("Start time", g_scheduleStartText, IM_ARRAYSIZE(g_scheduleStartText));
    DrawScheduleTimeInput("End time", g_scheduleEndText, IM_ARRAYSIZE(g_scheduleEndText));

    const char* currentPreset = (g_schedulePresetIndex >= 0 && g_schedulePresetIndex < Preset_GetCount())
        ? Preset_GetDisplayName(g_schedulePresetIndex)
        : "Select preset...";
    if (ImGui::BeginCombo("Preset to apply", currentPreset)) {
        for (int i = 0; i < Preset_GetCount(); ++i) {
            const bool selected = i == g_schedulePresetIndex;
            if (ImGui::Selectable(Preset_GetDisplayName(i), selected)) {
                g_schedulePresetIndex = i;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(88.0f);
    ImGui::InputInt("Blend minutes", &g_scheduleBlendMinutes);
    ImGui::SetNextItemWidth(88.0f);
    ImGui::InputInt("Blend seconds", &g_scheduleBlendSeconds);

    int startMinute = 0;
    int endMinute = 0;
    const bool startOk = PresetSchedule_ParseAmPm(g_scheduleStartText, startMinute);
    const bool endOk = PresetSchedule_ParseAmPm(g_scheduleEndText, endMinute);
    const bool durationOk = g_scheduleBlendMinutes >= 0 && g_scheduleBlendSeconds >= 0 && g_scheduleBlendSeconds < 60;
    const bool presetOk = g_schedulePresetIndex >= 0 && g_schedulePresetIndex < Preset_GetCount();
    const bool timeOk = startOk && endOk && startMinute != endMinute;
    const bool canSave = timeOk && durationOk && presetOk;

    if (!timeOk) {
        ImGui::TextColored(ImVec4(1.0f, 0.58f, 0.36f, 1.0f), "Use valid times like 6:00 PM, 18:00, 1800, or 6.30 PM.");
    } else if (!durationOk) {
        ImGui::TextColored(ImVec4(1.0f, 0.58f, 0.36f, 1.0f), "Blend seconds must be 0-59 and duration cannot be negative.");
    } else if (!presetOk) {
        ImGui::TextColored(ImVec4(1.0f, 0.58f, 0.36f, 1.0f), "Select an existing preset file.");
    } else {
        ImGui::TextDisabled("New entry wins: overlapping entries will be trimmed automatically.");
    }

    ImGui::Spacing();
    if (!canSave) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(g_scheduleEditingIndex >= 0 ? "Save" : "Add entry", ImVec2(108.0f, 0.0f))) {
        PresetScheduleEntry entry{};
        entry.startMinute = startMinute;
        entry.endMinute = endMinute;
        entry.presetFile = Preset_GetFileName(g_schedulePresetIndex);
        entry.blendSeconds = g_scheduleBlendMinutes * 60 + g_scheduleBlendSeconds;
        const bool saved = g_scheduleEditingIndex >= 0
            ? PresetSchedule_UpdateEntry(g_scheduleEditingIndex, entry)
            : PresetSchedule_AddEntry(entry);
        if (saved) {
            GUI_SetStatus(g_scheduleEditingIndex >= 0 ? "Time schedule entry updated" : "Time schedule entry added");
            ImGui::CloseCurrentPopup();
        } else {
            GUI_SetStatus("Time schedule entry failed");
        }
    }
    if (!canSave) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(88.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void DrawTimeScheduleSection() {
    ImGui::Spacing();
    ImGui::SeparatorText("Time Schedule");

    bool enabled = PresetSchedule_IsEnabled();
    if (ImGui::Checkbox("Enable time schedule", &enabled)) {
        PresetSchedule_SetEnabled(enabled);
    }
    const float addButtonWidth = ImGui::CalcTextSize("+ add").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine();
    const float addButtonX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - addButtonWidth;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), addButtonX));
    if (Preset_GetCount() <= 0) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("+ add")) {
        OpenTimeScheduleEntryPopup(nullptr);
    }
    if (Preset_GetCount() <= 0) {
        ImGui::EndDisabled();
    }

    int timeSource = PresetSchedule_GetTimeSource();
    if (ImGui::RadioButton("Use visual time override time", timeSource == 0)) {
        PresetSchedule_SetTimeSource(0);
        timeSource = 0;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Use in-game time", timeSource == 1)) {
        PresetSchedule_SetTimeSource(1);
    }

    const std::vector<PresetScheduleRow> rows = PresetSchedule_BuildRows();
    const PresetScheduleStatus scheduleStatus = PresetSchedule_GetStatus();
    DrawTimeScheduleTimeline(rows, scheduleStatus);
    if (rows.empty()) {
        ImGui::TextDisabled("No schedule rows");
    } else if (ImGui::BeginTable("TimeScheduleRows", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Range", ImGuiTableColumnFlags_WidthFixed, 158.0f);
        ImGui::TableSetupColumn("Preset");
        ImGui::TableSetupColumn("Edit", ImGuiTableColumnFlags_WidthFixed, 58.0f);
        ImGui::TableSetupColumn("Delete", ImGuiTableColumnFlags_WidthFixed, 64.0f);

        for (const PresetScheduleRow& row : rows) {
            ImGui::TableNextRow();
            if (scheduleStatus.enabled && !row.gap && row.entryIndex == scheduleStatus.activeEntryIndex) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(ImVec4(0.22f, 0.34f, 0.44f, 0.65f)));
            }
            ImGui::TableSetColumnIndex(0);
            const std::string rangeText = PresetSchedule_FormatAmPm(row.startMinute) + " -> " + PresetSchedule_FormatAmPm(row.endMinute);
            ImGui::TextUnformatted(rangeText.c_str());

            ImGui::TableSetColumnIndex(1);
            if (row.gap) {
                ImGui::TextDisabled("gap - no preset");
            } else if (row.presetMissing) {
                const std::string missing = row.displayName + " (missing)";
                ImGui::TextColored(ImVec4(1.0f, 0.38f, 0.34f, 1.0f), "%s", missing.c_str());
            } else {
                ImGui::TextUnformatted(row.displayName.c_str());
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::PushID(row.gap ? -1000 - row.startMinute : row.entryIndex);
            if (row.gap) {
                if (row.startMinute != row.endMinute && Preset_GetCount() > 0 && ImGui::Button("Set")) {
                    OpenTimeScheduleEntryPopup(&row);
                }
            } else if (ImGui::Button("Edit")) {
                OpenTimeScheduleEntryPopup(&row);
            }

            ImGui::TableSetColumnIndex(3);
            if (!row.gap) {
                if (ImGui::Button("Delete")) {
                    PresetSchedule_DeleteEntry(row.entryIndex);
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (g_schedulePopupOpenRequest) {
        ImGui::OpenPopup("Time Schedule Entry");
        g_schedulePopupOpenRequest = false;
    }
    DrawTimeScheduleEntryPopup();
}

void DrawPresetTab() {
    Preset_EnsureInitialized();

    if (DrawDisabledTabBody()) {
        return;
    }

    bool enabled = g_modEnabled.load();
    if (ImGui::Checkbox("Enabled", &enabled)) {
        SetModEnabled(enabled);
    }
    ImGui::Separator();

    const bool hasSelection = Preset_HasSelection();
    const char* selectedName = Preset_GetSelectedDisplayName();
    const int editRegion = Preset_GetEditRegion();

    const char* saveLabel = hasSelection ? "Save Changes" : "Create Preset";
    if (!Preset_CanSaveCurrent()) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(saveLabel, ImVec2(150.0f, 0.0f))) {
        if (Preset_HasSelection()) {
            Preset_SaveSelected();
        } else {
            strcpy_s(g_newPresetName, "NewPreset.ini");
            ImGui::OpenPopup("Save Preset");
        }
    }
    if (!Preset_CanSaveCurrent()) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Save As", ImVec2(92.0f, 0.0f))) {
        strcpy_s(g_newPresetName, hasSelection ? selectedName : "NewPreset.ini");
        ImGui::OpenPopup("Save Preset");
    }
    ImGui::SameLine();
    const char* resetLabel = editRegion == kPresetRegionGlobal ? "Reset Global to Defaults" : "Clear Region Overrides";
    if (ImGui::Button(resetLabel, ImVec2(176.0f, 0.0f))) {
        Preset_ResetEditRegion();
        g_activeWeather = -1;
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Presets");
    ImGui::SetNextItemWidth(-92.0f);
    ImGui::InputTextWithHint("##preset_filter", "Search...", g_presetFilter, IM_ARRAYSIZE(g_presetFilter));
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        Preset_Refresh();
        GUI_SetStatus("Preset list refreshed");
    }

    const float listHeight = 180.0f;
    if (ImGui::BeginChild("PresetLibrary", ImVec2(0.0f, listHeight), true)) {
        const bool scheduleEnabled = PresetSchedule_IsEnabled();
        const bool newVisible = TextContainsNoCase("[New Preset]", g_presetFilter);
        if (newVisible) {
            const bool newSelected = !scheduleEnabled && !hasSelection;
            if (ImGui::Selectable("[New Preset]", newSelected)) {
                Preset_SelectNew();
            }
            if (newSelected) {
                ImGui::SetItemDefaultFocus();
            }
        }

        int visibleCount = newVisible ? 1 : 0;
        const int count = Preset_GetCount();
        const int selectedIndex = Preset_GetSelectedIndex();
        for (int i = 0; i < count; ++i) {
            if (Preset_IsCommunityPreset(i)) {
                continue;
            }
            const char* name = Preset_GetDisplayName(i);
            if (!TextContainsNoCase(name, g_presetFilter)) {
                continue;
            }
            ++visibleCount;
            const bool selected = !scheduleEnabled && i == selectedIndex;
            if (ImGui::Selectable(name, selected)) {
                Preset_SelectIndex(i);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        if (visibleCount == 0) {
            ImGui::TextDisabled("No preset matches");
        }
    }
    ImGui::EndChild();

    ImGui::SeparatorText("Community Presets");
    Community_Tick();
    const float communityListHeight = 180.0f;
    if (ImGui::BeginChild("CommunityPresetLibrary", ImVec2(0.0f, communityListHeight), true)) {
        const bool scheduleEnabled = PresetSchedule_IsEnabled();
        int visibleCount = 0;
        const int count = Preset_GetCount();
        const int selectedIndex = Preset_GetSelectedIndex();
        for (int i = 0; i < count; ++i) {
            if (!Preset_IsCommunityPreset(i)) {
                continue;
            }
            const char* name = Preset_GetDisplayName(i);
            if (!TextContainsNoCase(name, g_presetFilter)) {
                continue;
            }
            ++visibleCount;
            const bool selected = !scheduleEnabled && i == selectedIndex;
            CommunityPresetUpdateStatus updateStatus = Community_GetPresetUpdateStatus(i);
            const bool showUpdate = updateStatus.updateAvailable && Community_IsEnabled();
            ImGui::PushID(i);
            if (showUpdate) {
                ImGui::SetNextItemWidth(-78.0f);
            }
            if (ImGui::Selectable(name, selected, 0, ImVec2(showUpdate ? max(1.0f, ImGui::GetContentRegionAvail().x - 76.0f) : 0.0f, 0.0f))) {
                Preset_SelectIndex(i);
            }
            if (showUpdate) {
                ImGui::SameLine();
                if (Community_IsBusy()) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::SmallButton("Update")) {
                    Community_RequestUpdateDownloadedPreset(i);
                }
                if (Community_IsBusy()) {
                    ImGui::EndDisabled();
                }
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        if (visibleCount == 0) {
            ImGui::TextDisabled("No community preset matches");
        }
    }
    ImGui::EndChild();

    if (ImGui::BeginPopupModal("Save Preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("File name", g_newPresetName, IM_ARRAYSIZE(g_newPresetName));
        if (ImGui::Button("Save")) {
            if (Preset_SaveAs(g_newPresetName)) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    DrawTimeScheduleSection();
}

} // namespace overlay_internal
