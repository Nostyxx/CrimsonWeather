#include "pch.h"

#include "overlay_bridge.h"
#include "preset_service.h"
#include "runtime_shared.h"

#include <imgui.h>
#include <reshade.hpp>

#include <cmath>
#include <cstdio>
#include <cctype>

namespace {

HMODULE g_overlayModule = nullptr;
bool g_overlayRegistered = false;
char g_newPresetName[128] = "NewPreset.ini";
char g_presetFilter[96] = "";
char g_timeEditText[32] = "";
int g_timeEditLastMinute = -1;
bool g_timeEditActive = false;
bool g_timeEditFocusRequest = false;
bool g_timeEditHadFocus = false;

int HourToMinuteOfDay(float hour) {
    const float normalized = NormalizeHour24(hour);
    int minutes = static_cast<int>(std::lround(normalized * 60.0f));
    minutes %= 24 * 60;
    if (minutes < 0) {
        minutes += 24 * 60;
    }
    return minutes;
}

float MinuteOfDayToHour(int minuteOfDay) {
    minuteOfDay %= 24 * 60;
    if (minuteOfDay < 0) {
        minuteOfDay += 24 * 60;
    }
    return static_cast<float>(minuteOfDay) / 60.0f;
}

void FormatGameClockFromHour(float hour, char* out, size_t outSize) {
    const int minuteOfDay = HourToMinuteOfDay(hour);
    const int rawHour = minuteOfDay / 60;
    const int minute = minuteOfDay % 60;
    const int displayHour = rawHour <= 12 ? rawHour : rawHour - 12;
    const char* meridiem = rawHour > 11 ? "PM" : "AM";
    sprintf_s(out, outSize, "%d:%02d %s", displayHour, minute, meridiem);
}

bool TryParseClockText(const char* text, int* outMinuteOfDay) {
    if (!text || !outMinuteOfDay) {
        return false;
    }

    while (*text && std::isspace(static_cast<unsigned char>(*text))) {
        ++text;
    }

    char* end = nullptr;
    long hour = strtol(text, &end, 10);
    if (end == text) {
        return false;
    }

    int minute = 0;
    text = end;
    while (*text && std::isspace(static_cast<unsigned char>(*text))) {
        ++text;
    }
    if (*text == ':') {
        ++text;
        char* minuteEnd = nullptr;
        long parsedMinute = strtol(text, &minuteEnd, 10);
        if (minuteEnd == text || parsedMinute < 0 || parsedMinute > 59) {
            return false;
        }
        minute = static_cast<int>(parsedMinute);
        text = minuteEnd;
    }

    while (*text && std::isspace(static_cast<unsigned char>(*text))) {
        ++text;
    }

    bool hasMeridiem = false;
    bool pm = false;
    if (*text) {
        const char c0 = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
        const char c1 = static_cast<char>(std::tolower(static_cast<unsigned char>(text[1])));
        if (c0 == 'a') {
            hasMeridiem = true;
            pm = false;
        } else if (c0 == 'p') {
            hasMeridiem = true;
            pm = true;
        } else {
            return false;
        }
        text += (c1 == 'm') ? 2 : 1;
        while (*text && std::isspace(static_cast<unsigned char>(*text))) {
            ++text;
        }
        if (*text) {
            return false;
        }
    }

    if (hasMeridiem) {
        if (hour < 1 || hour > 12) {
            return false;
        }
        hour %= 12;
        if (pm) {
            hour += 12;
        }
    } else {
        if (hour < 0 || hour > 23) {
            return false;
        }
    }

    *outMinuteOfDay = static_cast<int>(hour) * 60 + minute;
    return true;
}

} // namespace

void ApplyUiScale(float scale) {
}

namespace {

void DrawFeatureUnavailable(RuntimeFeatureId feature) {
    if (RuntimeFeatureAvailable(feature)) {
        return;
    }
    const char* note = RuntimeFeatureNote(feature);
    if (note && note[0]) {
        ImGui::TextDisabled("%s", note);
    }
}

bool DrawResetButton(const char* id) {
    ImGui::SameLine();
    return ImGui::Button(id, ImVec2(28.0f, 0.0f));
}

bool DrawSliderFloatRow(const char* label, const char* id, float* value, float minValue, float maxValue, const char* format) {
    ImGui::PushID(id);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    const float resetWidth = 28.0f;
    const float resetX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - resetWidth;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), resetX));
    const bool reset = ImGui::Button("R", ImVec2(resetWidth, 0.0f));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##value", value, minValue, maxValue, format);
    ImGui::PopID();
    return reset;
}

bool DrawSliderIntRow(const char* label, const char* id, int* value, int minValue, int maxValue, const char* format) {
    ImGui::PushID(id);
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    const float resetWidth = 28.0f;
    const float resetX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - resetWidth;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), resetX));
    const bool reset = ImGui::Button("R", ImVec2(resetWidth, 0.0f));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderInt("##value", value, minValue, maxValue, format);
    ImGui::PopID();
    return reset;
}

bool DrawClockDial(const char* id, int* minuteOfDay) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = kPi * 2.0f;
    constexpr float kDialSize = 116.0f;

    ImGui::PushID(id);
    const float availWidth = ImGui::GetContentRegionAvail().x;
    const float centeredX = ImGui::GetCursorPosX() + max(0.0f, (availWidth - kDialSize) * 0.5f);
    ImGui::SetCursorPosX(centeredX);
    ImGui::InvisibleButton("##clock", ImVec2(kDialSize, kDialSize));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    bool changed = false;
    const ImVec2 minPos = ImGui::GetItemRectMin();
    const ImVec2 maxPos = ImGui::GetItemRectMax();
    const ImVec2 center((minPos.x + maxPos.x) * 0.5f, (minPos.y + maxPos.y) * 0.5f);
    const float radius = kDialSize * 0.5f - 6.0f;

    if (active && ImGui::IsMouseDown(0)) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float dx = mouse.x - center.x;
        const float dy = mouse.y - center.y;
        if ((dx * dx + dy * dy) > 16.0f) {
            float angle = atan2f(dy, dx) + (kPi * 0.5f);
            if (angle < 0.0f) {
                angle += kTwoPi;
            }
            const int snapMinutes = ImGui::GetIO().KeyShift ? 1 : 5;
            const float rawMinutes = (angle / kTwoPi) * static_cast<float>(24 * 60);
            int snapped = static_cast<int>(std::lround(rawMinutes / snapMinutes)) * snapMinutes;
            snapped %= 24 * 60;
            if (snapped < 0) {
                snapped += 24 * 60;
            }
            if (*minuteOfDay != snapped) {
                *minuteOfDay = snapped;
                changed = true;
            }
        }
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 frameColor = ImGui::GetColorU32(ImGuiCol_FrameBg);
    const ImU32 borderColor = ImGui::GetColorU32(hovered || active ? ImGuiCol_ButtonHovered : ImGuiCol_Border);
    const ImU32 accentColor = ImGui::GetColorU32(active ? ImGuiCol_ButtonActive : ImGuiCol_Button);
    const ImU32 tickColor = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    draw->AddCircleFilled(center, radius, frameColor, 48);
    draw->AddCircle(center, radius, borderColor, 48, 1.5f);

    for (int i = 0; i < 24; ++i) {
        const bool major = (i % 6) == 0;
        const float angle = (static_cast<float>(i) / 24.0f) * kTwoPi - (kPi * 0.5f);
        const float tickOuter = radius - 3.0f;
        const float tickInner = tickOuter - (major ? 7.0f : 3.0f);
        const ImVec2 p1(center.x + cosf(angle) * tickInner, center.y + sinf(angle) * tickInner);
        const ImVec2 p2(center.x + cosf(angle) * tickOuter, center.y + sinf(angle) * tickOuter);
        draw->AddLine(p1, p2, tickColor, major ? 1.5f : 1.0f);
    }

    const float handAngle = (static_cast<float>(*minuteOfDay) / static_cast<float>(24 * 60)) * kTwoPi - (kPi * 0.5f);
    const ImVec2 handEnd(center.x + cosf(handAngle) * (radius - 18.0f), center.y + sinf(handAngle) * (radius - 18.0f));
    draw->AddLine(center, handEnd, accentColor, 2.5f);
    draw->AddCircleFilled(handEnd, 4.0f, accentColor, 16);
    draw->AddCircleFilled(center, 3.0f, ImGui::GetColorU32(ImGuiCol_Text), 16);

    ImGui::PopID();
    return changed;
}

bool TextContainsNoCase(const char* text, const char* needle) {
    if (!needle || !needle[0]) {
        return true;
    }
    if (!text) {
        return false;
    }

    for (const char* start = text; *start; ++start) {
        const char* h = start;
        const char* n = needle;
        while (*h && *n &&
            std::tolower(static_cast<unsigned char>(*h)) ==
            std::tolower(static_cast<unsigned char>(*n))) {
            ++h;
            ++n;
        }
        if (!*n) {
            return true;
        }
    }
    return false;
}

bool DrawStartupGate() {
    const AddonStartupState state = g_addonStartupState.load();
    if (state == AddonStartupState::Ready) {
        return false;
    }

    ImGui::Spacing();
    const StartupStepId step = g_startupStep.load();
    const bool failed = state == AddonStartupState::Failed;
    const char* headline = failed
        ? "Startup failed"
        : (state == AddonStartupState::Starting ? StartupStepLabel(step) : "Not started");
    if (failed) {
        ImGui::TextColored(ImVec4(1.0f, 0.32f, 0.28f, 1.0f), "%s", headline);
    } else {
        ImGui::TextUnformatted(headline);
    }

    const int stepCount = max(1, g_startupStepCount.load());
    const int stepIndex = max(0, min(stepCount, g_startupStepIndex.load()));
    const float progress = static_cast<float>(stepIndex) / static_cast<float>(stepCount);

    const ULONGLONG startTick = g_startupStartTick.load();
    const ULONGLONG endTick = g_startupEndTick.load();
    const ULONGLONG nowTick = GetTickCount64();
    const ULONGLONG elapsedMs = startTick ? ((endTick ? endTick : nowTick) - startTick) : 0;
    char progressLabel[48] = {};
    sprintf_s(progressLabel, "%.1fs", static_cast<double>(elapsedMs) / 1000.0);
    ImGui::ProgressBar(failed ? 1.0f : progress, ImVec2(-1.0f, 0.0f),
        state == AddonStartupState::NotStarted ? "" : progressLabel);
    if (failed) {
        ImGui::TextWrapped("%s", g_startupDetailText);
    }
    ImGui::Spacing();

    const bool canStart = state == AddonStartupState::NotStarted || state == AddonStartupState::Failed;
    if (!canStart) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button(state == AddonStartupState::Failed ? "Retry" : "Start Crimson Weather",
                      ImVec2(220.0f, 0.0f))) {
        RequestCrimsonWeatherStart();
    }
    if (!canStart) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Startup Log");
    const unsigned int seq = g_startupLogSequence.load();
    const unsigned int visible = min<unsigned int>(seq, kStartupLogLineCount);
    for (unsigned int i = 0; i < visible; ++i) {
        const unsigned int logical = seq - visible + i;
        const int slot = static_cast<int>(logical % kStartupLogLineCount);
        if (g_startupLogLines[slot][0]) {
            ImGui::TextUnformatted(g_startupLogLines[slot]);
        }
    }

    return true;
}

void DrawWindOnlyOverlay() {
    ImGui::Text("%s %s", MOD_DISPLAY_NAME, MOD_VERSION);
    const char* nexusLabel = "nexusmods";
    const float nexusWidth = ImGui::CalcTextSize(nexusLabel).x;
    const float cursorY = ImGui::GetCursorPosY() - ImGui::GetTextLineHeightWithSpacing();
    const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), rightEdge - nexusWidth));
    ImGui::SetCursorPosY(cursorY);
    ImGui::TextLinkOpenURL(nexusLabel, "https://www.nexusmods.com/crimsondesert/mods/632");
    ImGui::Separator();
    if (DrawStartupGate()) {
        return;
    }

    float wind = g_windMul.load();
    const bool windChanged = ImGui::SliderFloat("Wind", &wind, 0.0f, 15.0f, "x%.2f");
    if (DrawResetButton("R##wind_only")) {
        g_windMul.store(1.0f);
        SaveWindOnlyConfig();
    } else {
        const float clamped = min(15.0f, max(0.0f, wind));
        if (windChanged && fabsf(clamped - g_windMul.load()) > 0.0001f) {
            g_windMul.store(clamped);
            SaveWindOnlyConfig();
        }
    }
}

bool DrawDisabledTabBody() {
    if (g_modEnabled.load()) {
        return false;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Crimson Weather is currently disabled");
    ImGui::Spacing();
    if (ImGui::Button("Enable Crimson Weather", ImVec2(220.0f, 0.0f))) {
        SetModEnabled(true);
        GUI_SetStatus("Weather control enabled");
    }
    return true;
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
    if (ImGui::Button("Reset Sliders", ImVec2(112.0f, 0.0f))) {
        ResetAllSliders();
        g_activeWeather = -1;
        GUI_SetStatus("Sliders reset");
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Presets");
    ImGui::SetNextItemWidth(-92.0f);
    ImGui::InputTextWithHint("##preset_filter", "Search presets", g_presetFilter, IM_ARRAYSIZE(g_presetFilter));
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        Preset_Refresh();
        GUI_SetStatus("Preset list refreshed");
    }

    const float listHeight = min(210.0f, max(96.0f, ImGui::GetTextLineHeightWithSpacing() * 8.0f));
    if (ImGui::BeginChild("PresetLibrary", ImVec2(0.0f, listHeight), true)) {
        const bool newVisible = TextContainsNoCase("[New Preset]", g_presetFilter);
        if (newVisible) {
            const bool newSelected = !hasSelection;
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
            const char* name = Preset_GetDisplayName(i);
            if (!TextContainsNoCase(name, g_presetFilter)) {
                continue;
            }
            ++visibleCount;
            const bool selected = i == selectedIndex;
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

}

void DrawGeneralTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

    ImGui::SeparatorText("Weather");
    bool forceClear = g_forceClear.load();
    if (!RuntimeFeatureAvailable(RuntimeFeatureId::ForceClear)) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Checkbox("Force Clear Sky", &forceClear)) {
        g_forceClear.store(forceClear);
        GUI_SetStatus(forceClear ? "Force clear active" : "Force clear off");
    }
    if (!RuntimeFeatureAvailable(RuntimeFeatureId::ForceClear)) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::ForceClear);
    }

    float rain = g_oRain.active.load() ? g_oRain.value.load() : 0.0f;
    const bool rainEnabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Rain);
    if (!rainEnabled) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Rain", "rain", &rain, 0.0f, 1.0f, "%.3f")) {
        g_oRain.clear();
    } else if (rainEnabled) {
        rain > 0.0001f ? g_oRain.set(rain) : g_oRain.clear();
    }
    if (!rainEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::Rain);
    }

    float dust = g_oDust.active.load() ? g_oDust.value.load() : 0.0f;
    const bool dustEnabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Dust);
    if (!dustEnabled) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Dust", "dust", &dust, 0.0f, 2.0f, "%.3f")) {
        g_oDust.clear();
    } else if (dustEnabled) {
        if (dust > 0.0001f) {
            g_oDust.set(dust);
        } else {
            g_oDust.clear();
        }
    }
    if (!dustEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::Dust);
    }

    float snow = g_oSnow.active.load() ? g_oSnow.value.load() : 0.0f;
    const bool snowEnabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Snow);
    if (!snowEnabled) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Snow", "snow", &snow, 0.0f, 1.0f, "%.3f")) {
        g_oSnow.clear();
    } else if (snowEnabled) {
        snow > 0.0001f ? g_oSnow.set(snow) : g_oSnow.clear();
    }
    if (!snowEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::Snow);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Time");
    bool visualTimeOverride = g_timeCtrlActive.load() && g_timeFreeze.load();
    const bool timeEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::TimeControls) &&
                             g_timeLayoutReady.load();
    if (!timeEnabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Checkbox("Visual Time Override", &visualTimeOverride)) {
        g_timeCtrlActive.store(visualTimeOverride);
        g_timeFreeze.store(visualTimeOverride);
        g_timeApplyRequest.store(true);
        GUI_SetStatus(visualTimeOverride ? "Visual time override enabled" : "Visual time override disabled");
    }
    ImGui::SameLine();
    const float resetWidth = 28.0f;
    const float resetX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - resetWidth;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), resetX));
    const bool timeReset = ImGui::Button("R##time_reset", ImVec2(resetWidth, 0.0f));
    if (!timeEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::TimeControls);
    }

    int timeMinutes = HourToMinuteOfDay(g_timeTargetHour.load());
    char targetClock[32] = {};
    FormatGameClockFromHour(g_timeTargetHour.load(), targetClock, sizeof(targetClock));

    if (!(timeEnabled && visualTimeOverride)) {
        ImGui::BeginDisabled();
    }
    const bool timeChanged = DrawClockDial("time", &timeMinutes);
    const bool dialActive = ImGui::IsItemActive();

    if (timeChanged && timeEnabled && visualTimeOverride) {
        g_timeTargetHour.store(MinuteOfDayToHour(timeMinutes));
        g_timeCtrlActive.store(true);
        g_timeFreeze.store(true);
        g_timeApplyRequest.store(true);
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
        if (ImGui::IsItemClicked()) {
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
        } else {
            GUI_SetStatus("Invalid time");
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
        g_timeTargetHour.store(MinuteOfDayToHour(HourToMinuteOfDay(g_timeCurrentHour.load())));
        g_timeCtrlActive.store(true);
        g_timeFreeze.store(true);
        g_timeApplyRequest.store(true);
        g_timeEditActive = false;
        g_timeEditFocusRequest = false;
        g_timeEditHadFocus = false;
        g_timeEditLastMinute = -1;
    } else if (timeEnabled && visualTimeOverride && typedValid) {
        g_timeTargetHour.store(MinuteOfDayToHour(timeMinutes));
        g_timeCtrlActive.store(true);
        g_timeFreeze.store(true);
        g_timeApplyRequest.store(true);
    }
    if (!(timeEnabled && visualTimeOverride)) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Wind");
    const bool windEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::WindControls);
    float wind = g_windMul.load();
    if (!windEnabled) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Wind", "wind_general", &wind, 0.0f, 15.0f, "x%.2f")) {
        g_windMul.store(1.0f);
    } else if (windEnabled) {
        g_windMul.store(min(15.0f, max(0.0f, wind)));
    }
    if (!windEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::WindControls);
    }

    bool noWind = g_noWind.load();
    const bool noWindEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::NoWindControls);
    if (!noWindEnabled) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Checkbox("No Wind", &noWind)) {
        g_noWind.store(noWind);
        GUI_SetStatus(noWind ? "No Wind enabled" : "No Wind disabled");
    }
    if (!noWindEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::NoWindControls);
    }
}

void DrawAtmosphereTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

    const bool cloudEnabled = !g_forceClear.load() && RuntimeFeatureAvailable(RuntimeFeatureId::CloudControls);
    ImGui::SeparatorText("Clouds");
    if (!cloudEnabled) {
        ImGui::BeginDisabled();
    }

    float cloudHeight = g_oCloudSpdX.active.load() ? g_oCloudSpdX.value.load() : 1.0f;
    if (DrawSliderFloatRow("Cloud Height", "cloud_height", &cloudHeight, -20.0f, 20.0f, "%.2f")) {
        g_oCloudSpdX.clear();
    } else if (cloudEnabled) {
        g_oCloudSpdX.set(cloudHeight);
    }

    float cloudDensity = g_oCloudSpdY.active.load() ? g_oCloudSpdY.value.load() : 1.0f;
    if (DrawSliderFloatRow("Cloud Density", "cloud_density", &cloudDensity, 0.0f, 10.0f, "x%.2f")) {
        g_oCloudSpdY.clear();
    } else if (cloudEnabled) {
        fabsf(cloudDensity - 1.0f) <= 0.001f ? g_oCloudSpdY.clear() : g_oCloudSpdY.set(cloudDensity);
    }

    float midClouds = g_oHighClouds.active.load() ? g_oHighClouds.value.load() : 1.0f;
    if (DrawSliderFloatRow("Mid Clouds", "mid_clouds", &midClouds, 0.0f, 15.0f, "x%.2f")) {
        g_oHighClouds.clear();
    } else if (cloudEnabled) {
        fabsf(midClouds - 1.0f) <= 0.001f ? g_oHighClouds.clear() : g_oHighClouds.set(midClouds);
    }

    float highClouds = g_oAtmoAlpha.active.load() ? g_oAtmoAlpha.value.load() : 1.0f;
    if (DrawSliderFloatRow("High Clouds", "high_clouds", &highClouds, 0.0f, 15.0f, "x%.2f")) {
        g_oAtmoAlpha.clear();
    } else if (cloudEnabled) {
        fabsf(highClouds - 1.0f) <= 0.001f ? g_oAtmoAlpha.clear() : g_oAtmoAlpha.set(highClouds);
    }

    if (!cloudEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::CloudControls);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Atmosphere");

    float fogPct = 0.0f;
    if (g_oFog.active.load()) {
        const float fogN = sqrtf(min(1.0f, max(0.0f, g_oFog.value.load() / 100.0f)));
        fogPct = fogN * 100.0f;
    }

    const bool fogFeatureAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::FogControls);
    if (!fogFeatureAvailable) {
        ImGui::BeginDisabled();
    }
    const bool fogReset = DrawSliderFloatRow("Fog [LEGACY]", "fog_legacy", &fogPct, 0.0f, 100.0f, "%.1f%%");

    if (fogReset) {
        g_oFog.clear();
    } else if (fogFeatureAvailable) {
        const float t = fogPct * 0.01f;
        g_oFog.set(t * t * 100.0f);
    }

    if (!fogFeatureAvailable) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::FogControls);
    }

    float fogFromWind = g_oNativeFog.active.load() ? g_oNativeFog.value.load() : 0.0f;
    const bool windEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::WindControls);
    if (!windEnabled) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Fog", "fog_from_wind", &fogFromWind, 0.0f, 15.0f, "%.2f")) {
        g_oNativeFog.clear();
    } else if (windEnabled) {
        if (fogFromWind > 0.001f) {
            g_oNativeFog.set(fogFromWind);
        } else {
            g_oNativeFog.clear();
        }
    }
    if (!windEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::WindControls);
    }

}

void DrawExperimentTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

    ImGui::SeparatorText("Cloud Experiments");
    const bool enabled = !g_forceClear.load() && RuntimeFeatureAvailable(RuntimeFeatureId::ExperimentControls);
    if (!enabled) {
        ImGui::BeginDisabled();
    }

    float value2C = g_oExpCloud2C.active.load() ? g_oExpCloud2C.value.load() : 1.0f;
    if (DrawSliderFloatRow("2C", "2c", &value2C, 0.0f, 15.0f, "x%.2f")) {
        g_oExpCloud2C.clear();
    } else if (enabled) {
        fabsf(value2C - 1.0f) <= 0.001f ? g_oExpCloud2C.clear() : g_oExpCloud2C.set(value2C);
    }

    float value2D = g_oExpCloud2D.active.load() ? g_oExpCloud2D.value.load() : 1.0f;
    if (DrawSliderFloatRow("2D", "2d", &value2D, 0.0f, 15.0f, "x%.2f")) {
        g_oExpCloud2D.clear();
    } else if (enabled) {
        fabsf(value2D - 1.0f) <= 0.001f ? g_oExpCloud2D.clear() : g_oExpCloud2D.set(value2D);
    }

    float cloudVariation = g_oCloudVariation.active.load() ? g_oCloudVariation.value.load() : 1.0f;
    if (DrawSliderFloatRow("Cloud Variation [32]", "cloud_variation", &cloudVariation, 0.0f, 15.0f, "x%.2f")) {
        g_oCloudVariation.clear();
    } else if (enabled) {
        fabsf(cloudVariation - 1.0f) <= 0.001f ? g_oCloudVariation.clear() : g_oCloudVariation.set(cloudVariation);
    }

    float rotation = g_oExpNightSkyRot.active.load() ? g_oExpNightSkyRot.value.load() : 1.0f;
    if (DrawSliderFloatRow("Night Sky Rotation X [0A]", "rotation", &rotation, -15.0f, 15.0f, "%.2f")) {
        g_oExpNightSkyRot.clear();
    } else if (enabled) {
        fabsf(rotation - 1.0f) <= 0.001f ? g_oExpNightSkyRot.clear() : g_oExpNightSkyRot.set(rotation);
    }

    if (!enabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::ExperimentControls);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Details");
    float puddleScale = g_oCloudThk.active.load() ? g_oCloudThk.value.load() : 0.0f;
    const bool detailEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::DetailControls);
    if (!detailEnabled) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Puddle Scale", "puddle", &puddleScale, 0.0f, 1.0f, "%.3f")) {
        g_oCloudThk.clear();
    } else if (detailEnabled) {
        g_oCloudThk.set(puddleScale);
    }
    if (!detailEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::DetailControls);
    }
}

void DrawOverlay(reshade::api::effect_runtime*) {
#if !defined(CW_WIND_ONLY)
    Preset_OnWorldTick(g_pEnvManager && *g_pEnvManager != 0, 0.016f);
#endif

#if defined(CW_WIND_ONLY)
    DrawWindOnlyOverlay();
#else
    ImGui::SetNextWindowSize(ImVec2(620.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(MOD_NAME)) {
        ImGui::End();
        return;
    }

    ImGui::Text("%s %s", MOD_NAME, MOD_VERSION);
    const char* nexusLabel = "nexusmods";
    const float nexusWidth = ImGui::CalcTextSize(nexusLabel).x;
    const float cursorY = ImGui::GetCursorPosY() - ImGui::GetTextLineHeightWithSpacing();
    const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), rightEdge - nexusWidth));
    ImGui::SetCursorPosY(cursorY);
    ImGui::TextLinkOpenURL(nexusLabel, "https://www.nexusmods.com/crimsondesert/mods/632");
    ImGui::Spacing();
    if (DrawStartupGate()) {
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("cw_tabs")) {
        if (ImGui::BeginTabItem("Presets")) {
            DrawPresetTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("General")) {
            DrawGeneralTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Atmosphere")) {
            DrawAtmosphereTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Experiment")) {
            DrawExperimentTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::Text("Unsaved changes: %s", Preset_HasUnsavedChanges() ? "Yes" : "No");
    ImGui::End();
#endif
}

} // namespace

bool InitializeOverlayBridge(HMODULE module) {
    if (!reshade::register_addon(module)) {
        return false;
    }

    reshade::register_overlay(MOD_NAME, &DrawOverlay);
    g_overlayModule = module;
    g_overlayRegistered = true;
    return true;
}

void ShutdownOverlayBridge() {
    if (!g_overlayRegistered) {
        return;
    }

    reshade::unregister_overlay(MOD_NAME, &DrawOverlay);
    reshade::unregister_addon(g_overlayModule);
    g_overlayRegistered = false;
    g_overlayModule = nullptr;
}
