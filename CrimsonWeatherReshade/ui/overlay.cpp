#include "pch.h"

#include "overlay_bridge.h"
#include "moon_texture_override.h"
#include "preset_service.h"
#include "runtime_shared.h"

#include <imgui.h>
#include <reshade.hpp>

#include <cmath>
#include <cstdio>
#include <cctype>
#include <string>

namespace {

HMODULE g_overlayModule = nullptr;
bool g_overlayRegistered = false;
char g_newPresetName[128] = "NewPreset.ini";
char g_presetFilter[96] = "";
char g_moonTextureFilter[96] = "";
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

bool DrawOverrideToggle(bool* enabled) {
    if (!enabled) {
        return false;
    }

    const bool regionOverride = *enabled;
    const ImVec4 regionButton(0.20f, 0.34f, 0.62f, 1.0f);
    const ImVec4 regionButtonHover(0.26f, 0.42f, 0.74f, 1.0f);
    const ImVec4 globalButton(0.12f, 0.12f, 0.13f, 1.0f);
    const ImVec4 globalButtonHover(0.18f, 0.18f, 0.20f, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_Button, regionOverride ? regionButton : globalButton);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, regionOverride ? regionButtonHover : globalButtonHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, regionOverride ? regionButtonHover : globalButtonHover);
    const bool changed = ImGui::Button(regionOverride ? "O" : "G", ImVec2(28.0f, 0.0f));
    ImGui::PopStyleColor(3);
    if (changed) {
        *enabled = !*enabled;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(regionOverride
            ? "Region override active. Click to inherit Global."
            : "Using Global value. Click to override for this region.");
    }
    ImGui::SameLine();
    return changed;
}

void DrawOverrideBadge(bool regionOverride) {
    if (!regionOverride) {
        return;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.58f, 0.95f, 1.0f));
    ImGui::TextUnformatted("REGION");
    ImGui::PopStyleColor();
}

bool DrawSliderFloatRow(
    const char* label,
    const char* id,
    float* value,
    float minValue,
    float maxValue,
    const char* format,
    bool* outValueChanged = nullptr,
    bool* overrideEnabled = nullptr,
    bool* outOverrideChanged = nullptr,
    bool nativeDisplay = false) {
    ImGui::PushID(id);
    const bool overrideChanged = DrawOverrideToggle(overrideEnabled);
    const bool regionOverride = !overrideEnabled || *overrideEnabled;
    if (!regionOverride) {
        ImGui::BeginDisabled();
    }
    ImGui::TextUnformatted(label);
    if (!regionOverride) {
        ImGui::EndDisabled();
    }
    if (overrideEnabled) {
        DrawOverrideBadge(*overrideEnabled);
    }
    ImGui::SameLine();
    const float resetWidth = 28.0f;
    const float resetX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - resetWidth;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), resetX));
    const bool sliderDisabled = overrideEnabled && !*overrideEnabled;
    if (sliderDisabled) {
        ImGui::BeginDisabled();
    }
    const bool reset = ImGui::Button("R", ImVec2(resetWidth, 0.0f));
    if (sliderDisabled) {
        ImGui::EndDisabled();
    }
    ImGui::SetNextItemWidth(-1.0f);
    char inheritedFormat[64] = {};
    const char* displayFormat = format;
    if (nativeDisplay) {
        displayFormat = "NATIVE";
    }
    if (sliderDisabled) {
        sprintf_s(inheritedFormat, sizeof(inheritedFormat), "G: %s", nativeDisplay ? "NATIVE" : format);
        displayFormat = inheritedFormat;
        ImGui::BeginDisabled();
    }
    const bool valueChanged = ImGui::SliderFloat("##value", value, minValue, maxValue, displayFormat);
    if (sliderDisabled) {
        ImGui::EndDisabled();
    }
    if (outValueChanged) {
        *outValueChanged = valueChanged;
    }
    if (outOverrideChanged) {
        *outOverrideChanged = overrideChanged;
    }
    ImGui::PopID();
    return reset;
}

bool DrawOverrideCheckboxRow(
    const char* label,
    const char* id,
    bool* value,
    bool* overrideEnabled,
    bool* outOverrideChanged = nullptr) {
    ImGui::PushID(id);
    const bool overrideChanged = DrawOverrideToggle(overrideEnabled);
    const bool valueDisabled = overrideEnabled && !*overrideEnabled;
    if (valueDisabled) {
        ImGui::BeginDisabled();
    }
    const bool valueChanged = ImGui::Checkbox(label, value);
    if (valueDisabled) {
        ImGui::EndDisabled();
    }
    if (overrideEnabled) {
        DrawOverrideBadge(*overrideEnabled);
    }
    if (outOverrideChanged) {
        *outOverrideChanged = overrideChanged;
    }
    ImGui::PopID();
    return valueChanged;
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
    const ImVec2 dir(cosf(handAngle), sinf(handAngle));
    const ImVec2 perp(-dir.y, dir.x);
    const float arrowLen = 9.0f;
    const float arrowWide = 5.0f;
    const ImVec2 arrowBase(handEnd.x - dir.x * arrowLen, handEnd.y - dir.y * arrowLen);
    const ImVec2 arrowLeft(arrowBase.x + perp.x * arrowWide, arrowBase.y + perp.y * arrowWide);
    const ImVec2 arrowRight(arrowBase.x - perp.x * arrowWide, arrowBase.y - perp.y * arrowWide);
    draw->AddLine(center, arrowBase, accentColor, 2.5f);
    draw->AddTriangleFilled(handEnd, arrowLeft, arrowRight, accentColor);
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

void BuildEditScopeLabel(int regionId, char* out, size_t outSize) {
    if (regionId > kPresetRegionGlobal && Preset_SelectedHasRegion(regionId)) {
        sprintf_s(out, outSize, "%s *", Preset_GetRegionDisplayName(regionId));
    } else {
        sprintf_s(out, outSize, "%s", Preset_GetRegionDisplayName(regionId));
    }
}

void DrawEditScopeCombo() {
    Preset_EnsureInitialized();

    char selectedLabel[64] = {};
    int editRegion = Preset_GetEditRegion();
    BuildEditScopeLabel(editRegion, selectedLabel, sizeof(selectedLabel));

    const float comboWidth = 168.0f;
    const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), rightEdge - comboWidth));

    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo("##edit_scope", selectedLabel)) {
        for (int regionId = kPresetRegionGlobal; regionId < kPresetRegionCount; ++regionId) {
            char label[64] = {};
            BuildEditScopeLabel(regionId, label, sizeof(label));
            const bool selected = editRegion == regionId;
            if (ImGui::Selectable(label, selected)) {
                Preset_SetEditRegion(regionId);
                editRegion = regionId;
                GUI_SetStatus(("Editing preset scope: " + std::string(Preset_GetRegionDisplayName(regionId))).c_str());
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
}

std::string PresetStatusSourceLabel(const WeatherPresetStatusSnapshot& snapshot, bool regionOverride) {
    if (snapshot.blendFromRegion >= kPresetRegionGlobal &&
        snapshot.blendFromRegion < kPresetRegionCount &&
        snapshot.blendToRegion == snapshot.playerRegion &&
        snapshot.blendProgress < 0.999f) {
        return std::string("Blending: ") +
            Preset_GetRegionDisplayName(snapshot.blendFromRegion) +
            " -> " +
            Preset_GetRegionDisplayName(snapshot.blendToRegion);
    }
    if (!snapshot.hasPresetPackage) {
        return "Current runtime";
    }
    if (regionOverride) {
        return std::string("From ") + Preset_GetRegionDisplayName(snapshot.playerRegion);
    }
    return "From Global";
}

void DrawStatusRowText(
    const WeatherPresetStatusSnapshot& snapshot,
    const char* name,
    const char* value,
    bool regionOverride,
    const char* activeTooltip = nullptr) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(name);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(value);
    if (ImGui::IsItemHovered()) {
        if (strcmp(value, "NATIVE") == 0) {
            ImGui::SetTooltip("Crimson Weather is not overriding this; the game controls it natively.");
        } else if (strcmp(value, "DISABLED") == 0) {
            ImGui::SetTooltip("This Crimson Weather override is disabled.");
        } else if (activeTooltip && activeTooltip[0]) {
            ImGui::SetTooltip("%s", activeTooltip);
        }
    }
    ImGui::TableNextColumn();
    const std::string source = PresetStatusSourceLabel(snapshot, regionOverride);
    ImGui::TextUnformatted(source.c_str());
}

void DrawStatusRowFloat(
    const WeatherPresetStatusSnapshot& snapshot,
    const char* name,
    float value,
    const char* format,
    bool regionOverride,
    const char* activeTooltipFormat = nullptr) {
    char valueText[48] = {};
    sprintf_s(valueText, sizeof(valueText), format, value);
    char tooltipText[160] = {};
    if (activeTooltipFormat) {
        sprintf_s(tooltipText, sizeof(tooltipText), activeTooltipFormat, valueText);
    }
    DrawStatusRowText(snapshot, name, valueText, regionOverride, tooltipText);
}

void DrawStatusRowNativeFloat(
    const WeatherPresetStatusSnapshot& snapshot,
    const char* name,
    float value,
    float nativeValue,
    const char* format,
    bool regionOverride,
    const char* activeTooltipFormat) {
    if (fabsf(value - nativeValue) <= 0.0005f) {
        DrawStatusRowText(snapshot, name, "NATIVE", regionOverride);
        return;
    }
    DrawStatusRowFloat(snapshot, name, value, format, regionOverride, activeTooltipFormat);
}

void DrawStatusRowActiveFloat(
    const WeatherPresetStatusSnapshot& snapshot,
    const char* name,
    bool active,
    float value,
    const char* format,
    bool regionOverride,
    const char* activeTooltipFormat) {
    if (!active) {
        DrawStatusRowText(snapshot, name, "NATIVE", regionOverride);
        return;
    }
    DrawStatusRowFloat(snapshot, name, value, format, regionOverride, activeTooltipFormat);
}

void DrawStatusRowBool(
    const WeatherPresetStatusSnapshot& snapshot,
    const char* name,
    bool value,
    bool regionOverride,
    const char* activeTooltip) {
    DrawStatusRowText(snapshot, name, value ? "On" : "DISABLED", regionOverride, activeTooltip);
}

void DrawStatusRowBlocked(
    const WeatherPresetStatusSnapshot& snapshot,
    const char* name,
    bool regionOverride,
    const char* tooltip) {
    DrawStatusRowText(snapshot, name, "BLOCKED", regionOverride, tooltip);
}

void DrawStatusRowEnabledFloat(
    const WeatherPresetStatusSnapshot& snapshot,
    const char* name,
    bool enabled,
    float value,
    const char* format,
    bool regionOverride,
    const char* activeTooltipFormat) {
    char valueNumber[32] = {};
    char valueText[64] = {};
    sprintf_s(valueNumber, sizeof(valueNumber), format, value);
    char tooltipText[160] = {};
    if (enabled) {
        sprintf_s(valueText, sizeof(valueText), "On %s", valueNumber);
        sprintf_s(tooltipText, sizeof(tooltipText), activeTooltipFormat, valueNumber);
    } else {
        sprintf_s(valueText, sizeof(valueText), "NATIVE");
    }
    DrawStatusRowText(snapshot, name, valueText, regionOverride, tooltipText);
}

void DrawStatusTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

    const WeatherPresetStatusSnapshot snapshot = Preset_GetStatusSnapshot();
    ImGui::Text("Active Preset: %s", Preset_GetSelectedDisplayName());
    ImGui::Text("Player Region: %s", Preset_GetRegionDisplayName(snapshot.playerRegion));
    ImGui::Text("Editing: %s", Preset_GetRegionDisplayName(snapshot.editRegion));
    if (snapshot.blendFromRegion >= kPresetRegionGlobal &&
        snapshot.blendFromRegion < kPresetRegionCount &&
        snapshot.blendToRegion == snapshot.playerRegion &&
        snapshot.blendProgress < 0.999f) {
        ImGui::Text("Transition: %s -> %s %.0f%%",
            Preset_GetRegionDisplayName(snapshot.blendFromRegion),
            Preset_GetRegionDisplayName(snapshot.blendToRegion),
            snapshot.blendProgress * 100.0f);
    }

    ImGui::Spacing();
    if (ImGui::BeginTable(
            "PresetStatusTable",
            3,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Current");
        ImGui::TableSetupColumn("Source");
        ImGui::TableHeadersRow();

        DrawStatusRowBool(snapshot, "Force Clear Sky", snapshot.effective.forceClearSky, snapshot.regionSource.forceClearSky, "Crimson Weather currently forces clear skies.");
        if (snapshot.effective.forceClearSky) {
            DrawStatusRowBlocked(snapshot, "Rain", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so rain is not applied.");
            DrawStatusRowBlocked(snapshot, "Thunder", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so thunder is not applied.");
            DrawStatusRowBlocked(snapshot, "Dust", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so dust is not applied.");
            DrawStatusRowBlocked(snapshot, "Snow", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so snow is not applied.");
        } else {
            if (snapshot.effective.noRain) {
                DrawStatusRowBlocked(snapshot, "Rain", snapshot.regionSource.noRain, "No Rain is active, so Crimson Weather forces rain to zero.");
            } else {
                DrawStatusRowNativeFloat(snapshot, "Rain", snapshot.effective.rain, 0.0f, "%.3f", snapshot.regionSource.rain, "Crimson Weather currently forces rain to %s.");
            }
            DrawStatusRowNativeFloat(snapshot, "Thunder", snapshot.effective.thunder, 0.0f, "%.3f", snapshot.regionSource.thunder, "Crimson Weather currently drives visual lightning and thunder SFX frequency at %s.");
            if (snapshot.effective.noDust) {
                DrawStatusRowBlocked(snapshot, "Dust", snapshot.regionSource.noDust, "No Dust is active, so Crimson Weather forces dust to zero.");
            } else {
                DrawStatusRowNativeFloat(snapshot, "Dust", snapshot.effective.dust, 0.0f, "%.3f", snapshot.regionSource.dust, "Crimson Weather currently forces dust to %s.");
            }
            if (snapshot.effective.noSnow) {
                DrawStatusRowBlocked(snapshot, "Snow", snapshot.regionSource.noSnow, "No Snow is active, so Crimson Weather forces snow to zero.");
            } else {
                DrawStatusRowNativeFloat(snapshot, "Snow", snapshot.effective.snow, 0.0f, "%.3f", snapshot.regionSource.snow, "Crimson Weather currently forces snow to %s.");
            }
        }
        DrawStatusRowBool(snapshot, "No Rain", snapshot.effective.noRain, snapshot.regionSource.noRain, "Crimson Weather currently disables rain.");
        DrawStatusRowBool(snapshot, "No Dust", snapshot.effective.noDust, snapshot.regionSource.noDust, "Crimson Weather currently disables dust.");
        DrawStatusRowBool(snapshot, "No Snow", snapshot.effective.noSnow, snapshot.regionSource.noSnow, "Crimson Weather currently disables snow.");

        DrawStatusRowBool(snapshot, "Visual Time Override", snapshot.effective.visualTimeOverride, snapshot.regionSource.time, "Crimson Weather currently freezes visual time.");
        DrawStatusRowActiveFloat(snapshot, "Time Hour", snapshot.effective.visualTimeOverride, NormalizeHour24(snapshot.effective.timeHour), "%.2f", snapshot.regionSource.time, "Crimson Weather currently forces time to %s.");

        if (snapshot.effective.forceClearSky) {
            DrawStatusRowBlocked(snapshot, "Cloud Amount", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud amount overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Height", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud height overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Density", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud density overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Mid Clouds", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so mid-cloud overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "High Clouds", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so high-cloud overrides are not applied.");
        } else {
            DrawStatusRowEnabledFloat(snapshot, "Cloud Amount", snapshot.effective.cloudAmountEnabled, snapshot.effective.cloudAmount, "x%.2f", snapshot.regionSource.cloudAmount, "Crimson Weather currently multiplies cloud amount by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Height", snapshot.effective.cloudHeightEnabled, snapshot.effective.cloudHeight, "x%.2f", snapshot.regionSource.cloudHeight, "Crimson Weather currently multiplies cloud height by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Density", snapshot.effective.cloudDensityEnabled, snapshot.effective.cloudDensity, "x%.2f", snapshot.regionSource.cloudDensity, "Crimson Weather currently multiplies cloud density by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Mid Clouds", snapshot.effective.midCloudsEnabled, snapshot.effective.midClouds, "x%.2f", snapshot.regionSource.midClouds, "Crimson Weather currently multiplies mid clouds by %s.");
            DrawStatusRowEnabledFloat(snapshot, "High Clouds", snapshot.effective.highCloudsEnabled, snapshot.effective.highClouds, "x%.2f", snapshot.regionSource.highClouds, "Crimson Weather currently multiplies high clouds by %s.");
        }

        if (snapshot.effective.forceClearSky) {
            DrawStatusRowBlocked(snapshot, "2C", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so 2C cloud overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "2D", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so 2D cloud overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Variation", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud variation overrides are not applied.");
        } else {
            DrawStatusRowEnabledFloat(snapshot, "2C", snapshot.effective.exp2CEnabled, snapshot.effective.exp2C, "x%.2f", snapshot.regionSource.exp2C, "Crimson Weather currently multiplies 2C by %s.");
            DrawStatusRowEnabledFloat(snapshot, "2D", snapshot.effective.exp2DEnabled, snapshot.effective.exp2D, "x%.2f", snapshot.regionSource.exp2D, "Crimson Weather currently multiplies 2D by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Variation", snapshot.effective.cloudVariationEnabled, snapshot.effective.cloudVariation, "x%.2f", snapshot.regionSource.cloudVariation, "Crimson Weather currently multiplies cloud variation by %s.");
        }
        DrawStatusRowEnabledFloat(snapshot, "Puddle Scale", snapshot.effective.puddleScaleEnabled, snapshot.effective.puddleScale, "%.3f", snapshot.regionSource.puddleScale, "Crimson Weather currently sets puddle scale to %s.");

        DrawStatusRowEnabledFloat(snapshot, "Night Sky Tilt", snapshot.effective.nightSkyRotationEnabled, snapshot.effective.nightSkyRotation, "%.2f", snapshot.regionSource.nightSkyRotation, "Crimson Weather currently sets the native night sky tilt to %s.");
        DrawStatusRowEnabledFloat(snapshot, "Night Sky Phase", snapshot.effective.nightSkyYawEnabled, snapshot.effective.nightSkyYaw, "%.2f", snapshot.regionSource.nightSkyYaw, "Crimson Weather currently sets the native night sky phase to %s.");
        DrawStatusRowEnabledFloat(snapshot, "Sun Size", snapshot.effective.sunSizeEnabled, snapshot.effective.sunSize, "%.3f", snapshot.regionSource.sunSize, "Crimson Weather currently sets sun size to %s.");
        DrawStatusRowEnabledFloat(snapshot, "Sun Yaw Lock", snapshot.effective.sunYawEnabled, snapshot.effective.sunYaw, "%.2f", snapshot.regionSource.sunYaw, "Crimson Weather currently locks sun yaw to %s, overriding natural sun movement.");
        DrawStatusRowEnabledFloat(snapshot, "Sun Pitch Lock", snapshot.effective.sunPitchEnabled, snapshot.effective.sunPitch, "%.2f", snapshot.regionSource.sunPitch, "Crimson Weather currently locks sun pitch to %s, overriding natural sun movement.");
        DrawStatusRowEnabledFloat(snapshot, "Moon Size", snapshot.effective.moonSizeEnabled, snapshot.effective.moonSize, "%.3f", snapshot.regionSource.moonSize, "Crimson Weather currently sets moon size to %s.");
        DrawStatusRowEnabledFloat(snapshot, "Moon Yaw Lock", snapshot.effective.moonYawEnabled, snapshot.effective.moonYaw, "%.2f", snapshot.regionSource.moonYaw, "Crimson Weather currently locks moon yaw to %s, overriding natural moon movement.");
        DrawStatusRowEnabledFloat(snapshot, "Moon Pitch Lock", snapshot.effective.moonPitchEnabled, snapshot.effective.moonPitch, "%.2f", snapshot.regionSource.moonPitch, "Crimson Weather currently locks moon pitch to %s, overriding natural moon movement.");
        DrawStatusRowEnabledFloat(snapshot, "Moon Rotation", snapshot.effective.moonRollEnabled, snapshot.effective.moonRoll, "%.2f", snapshot.regionSource.moonRoll, "Crimson Weather currently rotates the moon disc to %s.");
        DrawStatusRowText(
            snapshot,
            "Moon Texture",
            (snapshot.effective.moonTextureEnabled && !snapshot.effective.moonTexture.empty()) ? snapshot.effective.moonTexture.c_str() : "NATIVE",
            snapshot.regionSource.moonTexture,
            "Crimson Weather currently swaps the moon texture.");

        const bool fogForcedZero = snapshot.effective.forceClearSky || snapshot.effective.noFog;
        const bool fogForceSource = snapshot.effective.forceClearSky ? snapshot.regionSource.forceClearSky : snapshot.regionSource.noFog;
        const char* fogForceTooltip = snapshot.effective.forceClearSky
            ? "Force Clear Sky is active, so Crimson Weather forces legacy fog to zero."
            : "No Fog is active, so Crimson Weather forces legacy fog to zero.";
        const char* nativeFogForceTooltip = snapshot.effective.forceClearSky
            ? "Force Clear Sky is active, so Crimson Weather forces native fog to zero."
            : "No Fog is active, so Crimson Weather forces native fog to zero.";
        if (fogForcedZero) {
            DrawStatusRowBlocked(snapshot, "Fog [LEGACY]", fogForceSource, fogForceTooltip);
        } else {
            DrawStatusRowEnabledFloat(snapshot, "Fog [LEGACY]", snapshot.effective.fogEnabled, snapshot.effective.fogPercent, "%.1f%%", snapshot.regionSource.fog, "Crimson Weather currently forces legacy fog to %s.");
        }
        if (fogForcedZero) {
            DrawStatusRowBlocked(snapshot, "Fog", fogForceSource, nativeFogForceTooltip);
        } else {
            DrawStatusRowEnabledFloat(snapshot, "Fog", snapshot.effective.nativeFogEnabled, snapshot.effective.nativeFog, "%.2f", snapshot.regionSource.nativeFog, "Crimson Weather currently scales native fog by %s.");
        }
        DrawStatusRowBool(snapshot, "No Fog", snapshot.effective.noFog, snapshot.regionSource.noFog, "Crimson Weather currently disables fog.");
        if (snapshot.effective.noWind) {
            DrawStatusRowBlocked(snapshot, "Wind", snapshot.regionSource.noWind, "No Wind is active, so the wind multiplier is not applied.");
        } else {
            DrawStatusRowNativeFloat(snapshot, "Wind", snapshot.effective.wind, 1.0f, "x%.2f", snapshot.regionSource.wind, "Crimson Weather currently multiplies wind by %s.");
        }
        DrawStatusRowBool(snapshot, "No Wind", snapshot.effective.noWind, snapshot.regionSource.noWind, "Crimson Weather currently disables wind.");

        ImGui::EndTable();
    }
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

    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData editData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask overrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    bool editChanged = false;

    ImGui::SeparatorText("Weather");
    bool forceClear = detachedEdit ? editData.forceClearSky : g_forceClear.load();
    if (!RuntimeFeatureAvailable(RuntimeFeatureId::ForceClear)) {
        ImGui::BeginDisabled();
    }
    bool forceClearOverrideChanged = false;
    const bool forceClearChanged = regionScoped
        ? DrawOverrideCheckboxRow("Force Clear Sky", "force_clear", &forceClear, &overrideMask.forceClearSky, &forceClearOverrideChanged)
        : ImGui::Checkbox("Force Clear Sky", &forceClear);
    if (forceClearOverrideChanged) {
        editChanged = true;
    }
    if (forceClearChanged) {
        if (detachedEdit) {
            editData.forceClearSky = forceClear;
            if (regionScoped) overrideMask.forceClearSky = true;
            editChanged = true;
        } else {
            g_forceClear.store(forceClear);
        }
        GUI_SetStatus(forceClear ? "Force clear active" : "Force clear off");
    }
    if (!RuntimeFeatureAvailable(RuntimeFeatureId::ForceClear)) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::ForceClear);
    }

    bool noRain = detachedEdit ? editData.noRain : g_noRain.load();
    bool noDust = detachedEdit ? editData.noDust : g_noDust.load();
    bool noSnow = detachedEdit ? editData.noSnow : g_noSnow.load();

    float rain = detachedEdit ? editData.rain : (g_oRain.active.load() ? g_oRain.value.load() : 0.0f);
    const bool rainNative = rain <= 0.0001f;
    const bool rainEnabled = !forceClear && !noRain && RuntimeFeatureAvailable(RuntimeFeatureId::Rain);
    if (!rainEnabled) {
        ImGui::BeginDisabled();
    }
    bool rainChanged = false;
    bool rainOverrideChanged = false;
    if (DrawSliderFloatRow("Rain", "rain", &rain, 0.0f, 1.0f, "%.3f", &rainChanged, regionScoped ? &overrideMask.rain : nullptr, &rainOverrideChanged, rainNative)) {
        if (detachedEdit) {
            editData.rain = 0.0f;
            if (regionScoped) overrideMask.rain = true;
            editChanged = true;
        } else {
            g_oRain.clear();
        }
    } else if (rainOverrideChanged) {
        editChanged = true;
    } else if (rainEnabled && rainChanged) {
        if (detachedEdit) {
            editData.rain = min(1.0f, max(0.0f, rain));
            if (regionScoped) overrideMask.rain = true;
            editChanged = true;
        } else {
            rain > 0.0001f ? g_oRain.set(rain) : g_oRain.clear();
        }
    }
    if (!rainEnabled) {
        ImGui::EndDisabled();
        if (!forceClear && !noRain) {
            DrawFeatureUnavailable(RuntimeFeatureId::Rain);
        }
    }

    float thunder = detachedEdit ? editData.thunder : (g_oThunder.active.load() ? g_oThunder.value.load() : 0.0f);
    const bool thunderNative = thunder <= 0.0001f;
    const bool thunderEnabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::ThunderControls);
    if (!thunderEnabled) {
        ImGui::BeginDisabled();
    }
    bool thunderChanged = false;
    bool thunderOverrideChanged = false;
    if (DrawSliderFloatRow("Thunder", "thunder", &thunder, 0.0f, 1.0f, "%.3f", &thunderChanged, regionScoped ? &overrideMask.thunder : nullptr, &thunderOverrideChanged, thunderNative)) {
        if (detachedEdit) {
            editData.thunder = 0.0f;
            if (regionScoped) overrideMask.thunder = true;
            editChanged = true;
        } else {
            g_oThunder.clear();
        }
    } else if (thunderOverrideChanged) {
        editChanged = true;
    } else if (thunderEnabled && thunderChanged) {
        if (detachedEdit) {
            editData.thunder = min(1.0f, max(0.0f, thunder));
            if (regionScoped) overrideMask.thunder = true;
            editChanged = true;
        } else {
            thunder > 0.0001f ? g_oThunder.set(thunder) : g_oThunder.clear();
        }
    }
    if (!thunderEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::ThunderControls);
    }

    float dust = detachedEdit ? editData.dust : (g_oDust.active.load() ? g_oDust.value.load() : 0.0f);
    const bool dustNative = dust <= 0.0001f;
    const bool dustEnabled = !forceClear && !noDust && RuntimeFeatureAvailable(RuntimeFeatureId::Dust);
    if (!dustEnabled) {
        ImGui::BeginDisabled();
    }
    bool dustChanged = false;
    bool dustOverrideChanged = false;
    if (DrawSliderFloatRow("Dust", "dust", &dust, 0.0f, 2.0f, "%.3f", &dustChanged, regionScoped ? &overrideMask.dust : nullptr, &dustOverrideChanged, dustNative)) {
        if (detachedEdit) {
            editData.dust = 0.0f;
            if (regionScoped) overrideMask.dust = true;
            editChanged = true;
        } else {
            g_oDust.clear();
        }
    } else if (dustOverrideChanged) {
        editChanged = true;
    } else if (dustEnabled && dustChanged) {
        if (detachedEdit) {
            editData.dust = min(2.0f, max(0.0f, dust));
            if (regionScoped) overrideMask.dust = true;
            editChanged = true;
        } else {
            if (dust > 0.0001f) {
                g_oDust.set(dust);
            } else {
                g_oDust.clear();
            }
        }
    }
    if (!dustEnabled) {
        ImGui::EndDisabled();
        if (!forceClear && !noDust) {
            DrawFeatureUnavailable(RuntimeFeatureId::Dust);
        }
    }

    float snow = detachedEdit ? editData.snow : (g_oSnow.active.load() ? g_oSnow.value.load() : 0.0f);
    const bool snowNative = snow <= 0.0001f;
    const bool snowEnabled = !forceClear && !noSnow && RuntimeFeatureAvailable(RuntimeFeatureId::Snow);
    if (!snowEnabled) {
        ImGui::BeginDisabled();
    }
    bool snowChanged = false;
    bool snowOverrideChanged = false;
    if (DrawSliderFloatRow("Snow", "snow", &snow, 0.0f, 1.0f, "%.3f", &snowChanged, regionScoped ? &overrideMask.snow : nullptr, &snowOverrideChanged, snowNative)) {
        if (detachedEdit) {
            editData.snow = 0.0f;
            if (regionScoped) overrideMask.snow = true;
            editChanged = true;
        } else {
            g_oSnow.clear();
        }
    } else if (snowOverrideChanged) {
        editChanged = true;
    } else if (snowEnabled && snowChanged) {
        if (detachedEdit) {
            editData.snow = min(1.0f, max(0.0f, snow));
            if (regionScoped) overrideMask.snow = true;
            editChanged = true;
        } else {
            snow > 0.0001f ? g_oSnow.set(snow) : g_oSnow.clear();
        }
    }
    if (!snowEnabled) {
        ImGui::EndDisabled();
        if (!forceClear && !noSnow) {
            DrawFeatureUnavailable(RuntimeFeatureId::Snow);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Disable Native Weather");
    const bool noRainEnabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Rain);
    if (!noRainEnabled) {
        ImGui::BeginDisabled();
    }
    bool noRainOverrideChanged = false;
    const bool noRainChanged = regionScoped
        ? DrawOverrideCheckboxRow("No Rain", "no_rain", &noRain, &overrideMask.noRain, &noRainOverrideChanged)
        : ImGui::Checkbox("No Rain", &noRain);
    if (noRainOverrideChanged) {
        editChanged = true;
    }
    if (noRainChanged) {
        if (detachedEdit) {
            editData.noRain = noRain;
            if (regionScoped) overrideMask.noRain = true;
            editChanged = true;
        } else {
            g_noRain.store(noRain);
        }
        GUI_SetStatus(noRain ? "No Rain enabled" : "No Rain disabled");
    }
    if (!noRainEnabled) {
        ImGui::EndDisabled();
    }

    const bool noDustEnabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Dust);
    if (!noDustEnabled) {
        ImGui::BeginDisabled();
    }
    bool noDustOverrideChanged = false;
    const bool noDustChanged = regionScoped
        ? DrawOverrideCheckboxRow("No Dust", "no_dust", &noDust, &overrideMask.noDust, &noDustOverrideChanged)
        : ImGui::Checkbox("No Dust", &noDust);
    if (noDustOverrideChanged) {
        editChanged = true;
    }
    if (noDustChanged) {
        if (detachedEdit) {
            editData.noDust = noDust;
            if (regionScoped) overrideMask.noDust = true;
            editChanged = true;
        } else {
            g_noDust.store(noDust);
        }
        GUI_SetStatus(noDust ? "No Dust enabled" : "No Dust disabled");
    }
    if (!noDustEnabled) {
        ImGui::EndDisabled();
    }

    const bool noSnowEnabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Snow);
    if (!noSnowEnabled) {
        ImGui::BeginDisabled();
    }
    bool noSnowOverrideChanged = false;
    const bool noSnowChanged = regionScoped
        ? DrawOverrideCheckboxRow("No Snow", "no_snow", &noSnow, &overrideMask.noSnow, &noSnowOverrideChanged)
        : ImGui::Checkbox("No Snow", &noSnow);
    if (noSnowOverrideChanged) {
        editChanged = true;
    }
    if (noSnowChanged) {
        if (detachedEdit) {
            editData.noSnow = noSnow;
            if (regionScoped) overrideMask.noSnow = true;
            editChanged = true;
        } else {
            g_noSnow.store(noSnow);
        }
        GUI_SetStatus(noSnow ? "No Snow enabled" : "No Snow disabled");
    }
    if (!noSnowEnabled) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Time");
    bool visualTimeOverride = detachedEdit ? editData.visualTimeOverride : (g_timeCtrlActive.load() && g_timeFreeze.load());
    const bool timeEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::TimeControls) &&
                             g_timeLayoutReady.load();
    if (!timeEnabled) {
        ImGui::BeginDisabled();
    }
    bool timeOverrideChanged = false;
    const bool visualTimeChanged = regionScoped
        ? DrawOverrideCheckboxRow("Visual Time Override", "visual_time", &visualTimeOverride, &overrideMask.time, &timeOverrideChanged)
        : ImGui::Checkbox("Visual Time Override", &visualTimeOverride);
    if (timeOverrideChanged) {
        editChanged = true;
    }
    if (visualTimeChanged) {
        if (detachedEdit) {
            editData.visualTimeOverride = visualTimeOverride;
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeCtrlActive.store(visualTimeOverride);
            g_timeFreeze.store(visualTimeOverride);
            g_timeApplyRequest.store(true);
        }
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

    int timeMinutes = HourToMinuteOfDay(detachedEdit ? editData.timeHour : g_timeTargetHour.load());
    char targetClock[32] = {};
    FormatGameClockFromHour(detachedEdit ? editData.timeHour : g_timeTargetHour.load(), targetClock, sizeof(targetClock));

    const bool timeValueLocked = regionScoped && !overrideMask.time;
    if (!(timeEnabled && visualTimeOverride) || timeValueLocked) {
        ImGui::BeginDisabled();
    }
    const bool timeChanged = DrawClockDial("time", &timeMinutes);
    const bool dialActive = ImGui::IsItemActive();

    if (timeChanged && timeEnabled && visualTimeOverride) {
        if (detachedEdit) {
            editData.timeHour = MinuteOfDayToHour(timeMinutes);
            editData.visualTimeOverride = true;
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeTargetHour.store(MinuteOfDayToHour(timeMinutes));
            g_timeCtrlActive.store(true);
            g_timeFreeze.store(true);
            g_timeApplyRequest.store(true);
        }
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
        if (detachedEdit) {
            editData.timeHour = MinuteOfDayToHour(HourToMinuteOfDay(g_timeCurrentHour.load()));
            editData.visualTimeOverride = true;
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeTargetHour.store(MinuteOfDayToHour(HourToMinuteOfDay(g_timeCurrentHour.load())));
            g_timeCtrlActive.store(true);
            g_timeFreeze.store(true);
            g_timeApplyRequest.store(true);
        }
        g_timeEditActive = false;
        g_timeEditFocusRequest = false;
        g_timeEditHadFocus = false;
        g_timeEditLastMinute = -1;
    } else if (timeEnabled && visualTimeOverride && typedValid) {
        if (detachedEdit) {
            editData.timeHour = MinuteOfDayToHour(timeMinutes);
            editData.visualTimeOverride = true;
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeTargetHour.store(MinuteOfDayToHour(timeMinutes));
            g_timeCtrlActive.store(true);
            g_timeFreeze.store(true);
            g_timeApplyRequest.store(true);
        }
    }
    if (!(timeEnabled && visualTimeOverride) || timeValueLocked) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Wind");
    bool noWind = detachedEdit ? editData.noWind : g_noWind.load();
    const bool windEnabled = !noWind && RuntimeFeatureAvailable(RuntimeFeatureId::WindControls);
    float wind = detachedEdit ? editData.wind : g_windMul.load();
    const bool windNative = fabsf(wind - 1.0f) <= 0.001f;
    if (!windEnabled) {
        ImGui::BeginDisabled();
    }
    bool windChanged = false;
    bool windOverrideChanged = false;
    if (DrawSliderFloatRow("Wind", "wind_general", &wind, 0.0f, 15.0f, "x%.2f", &windChanged, regionScoped ? &overrideMask.wind : nullptr, &windOverrideChanged, windNative)) {
        if (detachedEdit) {
            editData.wind = 1.0f;
            if (regionScoped) overrideMask.wind = true;
            editChanged = true;
        } else {
            g_windMul.store(1.0f);
        }
    } else if (windOverrideChanged) {
        editChanged = true;
    } else if (windEnabled && windChanged) {
        if (detachedEdit) {
            editData.wind = min(15.0f, max(0.0f, wind));
            if (regionScoped) overrideMask.wind = true;
            editChanged = true;
        } else {
            g_windMul.store(min(15.0f, max(0.0f, wind)));
        }
    }
    if (!windEnabled) {
        ImGui::EndDisabled();
        if (!noWind) {
            DrawFeatureUnavailable(RuntimeFeatureId::WindControls);
        }
    }

    const bool noWindEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::NoWindControls);
    if (!noWindEnabled) {
        ImGui::BeginDisabled();
    }
    bool noWindOverrideChanged = false;
    const bool noWindChanged = regionScoped
        ? DrawOverrideCheckboxRow("No Wind", "no_wind", &noWind, &overrideMask.noWind, &noWindOverrideChanged)
        : ImGui::Checkbox("No Wind", &noWind);
    if (noWindOverrideChanged) {
        editChanged = true;
    }
    if (noWindChanged) {
        if (detachedEdit) {
            editData.noWind = noWind;
            if (regionScoped) overrideMask.noWind = true;
            editChanged = true;
        } else {
            g_noWind.store(noWind);
        }
        GUI_SetStatus(noWind ? "No Wind enabled" : "No Wind disabled");
    }
    if (!noWindEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::NoWindControls);
    }

    if (detachedEdit && editChanged) {
        if (regionScoped) {
            Preset_SetEditRegionDataWithOverrides(editData, overrideMask);
        } else {
            Preset_SetEditRegionData(editData);
        }
    }
}

void DrawAtmosphereTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData editData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask overrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    bool editChanged = false;

    const bool cloudEnabled = !(detachedEdit ? editData.forceClearSky : g_forceClear.load()) && RuntimeFeatureAvailable(RuntimeFeatureId::CloudControls);
    ImGui::SeparatorText("Clouds");
    if (!cloudEnabled) {
        ImGui::BeginDisabled();
    }

    float cloudAmount = detachedEdit ? editData.cloudAmount : (g_oCloudAmount.active.load() ? g_oCloudAmount.value.load() : 1.0f);
    const bool cloudAmountNative = detachedEdit ? !editData.cloudAmountEnabled : !g_oCloudAmount.active.load();
    bool cloudAmountChanged = false;
    bool cloudAmountOverrideChanged = false;
    if (DrawSliderFloatRow("Cloud Amount", "cloud_amount", &cloudAmount, 0.0f, 15.0f, "x%.2f", &cloudAmountChanged, regionScoped ? &overrideMask.cloudAmount : nullptr, &cloudAmountOverrideChanged, cloudAmountNative)) {
        if (detachedEdit) {
            editData.cloudAmountEnabled = false;
            editData.cloudAmount = 1.0f;
            if (regionScoped) overrideMask.cloudAmount = true;
            editChanged = true;
        } else {
            g_oCloudAmount.clear();
        }
    } else if (cloudAmountOverrideChanged) {
        editChanged = true;
    } else if (cloudEnabled && cloudAmountChanged) {
        if (detachedEdit) {
            editData.cloudAmount = min(15.0f, max(0.0f, cloudAmount));
            editData.cloudAmountEnabled = fabsf(editData.cloudAmount - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.cloudAmount = true;
            editChanged = true;
        } else {
            fabsf(cloudAmount - 1.0f) <= 0.001f ? g_oCloudAmount.clear() : g_oCloudAmount.set(cloudAmount);
        }
    }

    float cloudHeight = detachedEdit ? editData.cloudHeight : (g_oCloudSpdX.active.load() ? g_oCloudSpdX.value.load() : 1.0f);
    const bool cloudHeightNative = detachedEdit ? !editData.cloudHeightEnabled : !g_oCloudSpdX.active.load();
    bool cloudHeightChanged = false;
    bool cloudHeightOverrideChanged = false;
    if (DrawSliderFloatRow("Cloud Height", "cloud_height", &cloudHeight, -15.0f, 15.0f, "x%.2f", &cloudHeightChanged, regionScoped ? &overrideMask.cloudHeight : nullptr, &cloudHeightOverrideChanged, cloudHeightNative)) {
        if (detachedEdit) {
            editData.cloudHeightEnabled = false;
            editData.cloudHeight = 1.0f;
            if (regionScoped) overrideMask.cloudHeight = true;
            editChanged = true;
        } else {
            g_oCloudSpdX.clear();
        }
    } else if (cloudHeightOverrideChanged) {
        editChanged = true;
    } else if (cloudEnabled && cloudHeightChanged) {
        if (detachedEdit) {
            editData.cloudHeight = min(15.0f, max(-15.0f, cloudHeight));
            editData.cloudHeightEnabled = fabsf(editData.cloudHeight - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.cloudHeight = true;
            editChanged = true;
        } else {
            fabsf(cloudHeight - 1.0f) <= 0.001f ? g_oCloudSpdX.clear() : g_oCloudSpdX.set(cloudHeight);
        }
    }

    float cloudDensity = detachedEdit ? editData.cloudDensity : (g_oCloudSpdY.active.load() ? g_oCloudSpdY.value.load() : 1.0f);
    const bool cloudDensityNative = detachedEdit ? !editData.cloudDensityEnabled : !g_oCloudSpdY.active.load();
    bool cloudDensityChanged = false;
    bool cloudDensityOverrideChanged = false;
    if (DrawSliderFloatRow("Cloud Density", "cloud_density", &cloudDensity, 0.0f, 10.0f, "x%.2f", &cloudDensityChanged, regionScoped ? &overrideMask.cloudDensity : nullptr, &cloudDensityOverrideChanged, cloudDensityNative)) {
        if (detachedEdit) {
            editData.cloudDensityEnabled = false;
            editData.cloudDensity = 1.0f;
            if (regionScoped) overrideMask.cloudDensity = true;
            editChanged = true;
        } else {
            g_oCloudSpdY.clear();
        }
    } else if (cloudDensityOverrideChanged) {
        editChanged = true;
    } else if (cloudEnabled && cloudDensityChanged) {
        if (detachedEdit) {
            editData.cloudDensity = min(10.0f, max(0.0f, cloudDensity));
            editData.cloudDensityEnabled = fabsf(editData.cloudDensity - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.cloudDensity = true;
            editChanged = true;
        } else {
            fabsf(cloudDensity - 1.0f) <= 0.001f ? g_oCloudSpdY.clear() : g_oCloudSpdY.set(cloudDensity);
        }
    }

    float midClouds = detachedEdit ? editData.midClouds : (g_oHighClouds.active.load() ? g_oHighClouds.value.load() : 1.0f);
    const bool midCloudsNative = detachedEdit ? !editData.midCloudsEnabled : !g_oHighClouds.active.load();
    bool midCloudsChanged = false;
    bool midCloudsOverrideChanged = false;
    if (DrawSliderFloatRow("Mid Clouds", "mid_clouds", &midClouds, 0.0f, 15.0f, "x%.2f", &midCloudsChanged, regionScoped ? &overrideMask.midClouds : nullptr, &midCloudsOverrideChanged, midCloudsNative)) {
        if (detachedEdit) {
            editData.midCloudsEnabled = false;
            editData.midClouds = 1.0f;
            if (regionScoped) overrideMask.midClouds = true;
            editChanged = true;
        } else {
            g_oHighClouds.clear();
        }
    } else if (midCloudsOverrideChanged) {
        editChanged = true;
    } else if (cloudEnabled && midCloudsChanged) {
        if (detachedEdit) {
            editData.midClouds = min(15.0f, max(0.0f, midClouds));
            editData.midCloudsEnabled = fabsf(editData.midClouds - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.midClouds = true;
            editChanged = true;
        } else {
            fabsf(midClouds - 1.0f) <= 0.001f ? g_oHighClouds.clear() : g_oHighClouds.set(midClouds);
        }
    }

    float highClouds = detachedEdit ? editData.highClouds : (g_oAtmoAlpha.active.load() ? g_oAtmoAlpha.value.load() : 1.0f);
    const bool highCloudsNative = detachedEdit ? !editData.highCloudsEnabled : !g_oAtmoAlpha.active.load();
    bool highCloudsChanged = false;
    bool highCloudsOverrideChanged = false;
    if (DrawSliderFloatRow("High Clouds", "high_clouds", &highClouds, 0.0f, 15.0f, "x%.2f", &highCloudsChanged, regionScoped ? &overrideMask.highClouds : nullptr, &highCloudsOverrideChanged, highCloudsNative)) {
        if (detachedEdit) {
            editData.highCloudsEnabled = false;
            editData.highClouds = 1.0f;
            if (regionScoped) overrideMask.highClouds = true;
            editChanged = true;
        } else {
            g_oAtmoAlpha.clear();
        }
    } else if (highCloudsOverrideChanged) {
        editChanged = true;
    } else if (cloudEnabled && highCloudsChanged) {
        if (detachedEdit) {
            editData.highClouds = min(15.0f, max(0.0f, highClouds));
            editData.highCloudsEnabled = fabsf(editData.highClouds - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.highClouds = true;
            editChanged = true;
        } else {
            fabsf(highClouds - 1.0f) <= 0.001f ? g_oAtmoAlpha.clear() : g_oAtmoAlpha.set(highClouds);
        }
    }

    if (!cloudEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::CloudControls);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Atmosphere");

    float fogFromWind = detachedEdit ? editData.nativeFog : (g_oNativeFog.active.load() ? g_oNativeFog.value.load() : 1.0f);
    const bool fogFromWindNative = detachedEdit ? !editData.nativeFogEnabled : !g_oNativeFog.active.load();
    const bool fogBlocked = detachedEdit ? (editData.forceClearSky || editData.noFog) : (g_forceClear.load() || g_noFog.load());
    const bool windEnabled = !fogBlocked && RuntimeFeatureAvailable(RuntimeFeatureId::WindControls);
    if (!windEnabled) {
        ImGui::BeginDisabled();
    }
    bool fogFromWindChanged = false;
    bool fogFromWindOverrideChanged = false;
    if (DrawSliderFloatRow("Fog", "fog_from_wind", &fogFromWind, 0.0f, 15.0f, "%.2f", &fogFromWindChanged, regionScoped ? &overrideMask.nativeFog : nullptr, &fogFromWindOverrideChanged, fogFromWindNative)) {
        if (detachedEdit) {
            editData.nativeFogEnabled = false;
            editData.nativeFog = 1.0f;
            if (regionScoped) overrideMask.nativeFog = true;
            editChanged = true;
        } else {
            g_oNativeFog.clear();
        }
    } else if (fogFromWindOverrideChanged) {
        editChanged = true;
    } else if (windEnabled && fogFromWindChanged) {
        if (detachedEdit) {
            editData.nativeFog = min(15.0f, max(0.0f, fogFromWind));
            editData.nativeFogEnabled = fabsf(editData.nativeFog - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.nativeFog = true;
            editChanged = true;
        } else {
            if (fabsf(fogFromWind - 1.0f) > 0.001f) {
                g_oNativeFog.set(fogFromWind);
            } else {
                g_oNativeFog.clear();
            }
        }
    }
    if (!windEnabled) {
        ImGui::EndDisabled();
        if (!fogBlocked) {
            DrawFeatureUnavailable(RuntimeFeatureId::WindControls);
        }
    }

    bool noFog = detachedEdit ? editData.noFog : g_noFog.load();
    bool noFogOverrideChanged = false;
    const bool noFogChanged = regionScoped
        ? DrawOverrideCheckboxRow("No Fog", "no_fog", &noFog, &overrideMask.noFog, &noFogOverrideChanged)
        : ImGui::Checkbox("No Fog", &noFog);
    if (noFogOverrideChanged) {
        editChanged = true;
    }
    if (noFogChanged) {
        if (detachedEdit) {
            editData.noFog = noFog;
            if (regionScoped) overrideMask.noFog = true;
            editChanged = true;
        } else {
            g_noFog.store(noFog);
        }
        GUI_SetStatus(noFog ? "No Fog enabled" : "No Fog disabled");
    }

    if (detachedEdit && editChanged) {
        if (regionScoped) {
            Preset_SetEditRegionDataWithOverrides(editData, overrideMask);
        } else {
            Preset_SetEditRegionData(editData);
        }
    }
}

void DrawExperimentTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData editData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask overrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    bool editChanged = false;

    ImGui::SeparatorText("Cloud Experiments");
    const bool enabled = !(detachedEdit ? editData.forceClearSky : g_forceClear.load()) && RuntimeFeatureAvailable(RuntimeFeatureId::ExperimentControls);
    if (!enabled) {
        ImGui::BeginDisabled();
    }

    float value2C = detachedEdit ? editData.exp2C : (g_oExpCloud2C.active.load() ? g_oExpCloud2C.value.load() : 1.0f);
    const bool value2CNative = detachedEdit ? !editData.exp2CEnabled : !g_oExpCloud2C.active.load();
    bool value2CChanged = false;
    bool value2COverrideChanged = false;
    if (DrawSliderFloatRow("2C", "2c", &value2C, 0.0f, 15.0f, "x%.2f", &value2CChanged, regionScoped ? &overrideMask.exp2C : nullptr, &value2COverrideChanged, value2CNative)) {
        if (detachedEdit) {
            editData.exp2CEnabled = false;
            editData.exp2C = 1.0f;
            if (regionScoped) overrideMask.exp2C = true;
            editChanged = true;
        } else {
            g_oExpCloud2C.clear();
        }
    } else if (value2COverrideChanged) {
        editChanged = true;
    } else if (enabled && value2CChanged) {
        if (detachedEdit) {
            editData.exp2C = min(15.0f, max(0.0f, value2C));
            editData.exp2CEnabled = fabsf(editData.exp2C - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.exp2C = true;
            editChanged = true;
        } else {
            fabsf(value2C - 1.0f) <= 0.001f ? g_oExpCloud2C.clear() : g_oExpCloud2C.set(value2C);
        }
    }

    float value2D = detachedEdit ? editData.exp2D : (g_oExpCloud2D.active.load() ? g_oExpCloud2D.value.load() : 1.0f);
    const bool value2DNative = detachedEdit ? !editData.exp2DEnabled : !g_oExpCloud2D.active.load();
    bool value2DChanged = false;
    bool value2DOverrideChanged = false;
    if (DrawSliderFloatRow("2D", "2d", &value2D, 0.0f, 15.0f, "x%.2f", &value2DChanged, regionScoped ? &overrideMask.exp2D : nullptr, &value2DOverrideChanged, value2DNative)) {
        if (detachedEdit) {
            editData.exp2DEnabled = false;
            editData.exp2D = 1.0f;
            if (regionScoped) overrideMask.exp2D = true;
            editChanged = true;
        } else {
            g_oExpCloud2D.clear();
        }
    } else if (value2DOverrideChanged) {
        editChanged = true;
    } else if (enabled && value2DChanged) {
        if (detachedEdit) {
            editData.exp2D = min(15.0f, max(0.0f, value2D));
            editData.exp2DEnabled = fabsf(editData.exp2D - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.exp2D = true;
            editChanged = true;
        } else {
            fabsf(value2D - 1.0f) <= 0.001f ? g_oExpCloud2D.clear() : g_oExpCloud2D.set(value2D);
        }
    }

    float cloudVariation = detachedEdit ? editData.cloudVariation : (g_oCloudVariation.active.load() ? g_oCloudVariation.value.load() : 1.0f);
    const bool cloudVariationNative = detachedEdit ? !editData.cloudVariationEnabled : !g_oCloudVariation.active.load();
    bool cloudVariationChanged = false;
    bool cloudVariationOverrideChanged = false;
    if (DrawSliderFloatRow("Cloud Variation [32]", "cloud_variation", &cloudVariation, 0.0f, 15.0f, "x%.2f", &cloudVariationChanged, regionScoped ? &overrideMask.cloudVariation : nullptr, &cloudVariationOverrideChanged, cloudVariationNative)) {
        if (detachedEdit) {
            editData.cloudVariationEnabled = false;
            editData.cloudVariation = 1.0f;
            if (regionScoped) overrideMask.cloudVariation = true;
            editChanged = true;
        } else {
            g_oCloudVariation.clear();
        }
    } else if (cloudVariationOverrideChanged) {
        editChanged = true;
    } else if (enabled && cloudVariationChanged) {
        if (detachedEdit) {
            editData.cloudVariation = min(15.0f, max(0.0f, cloudVariation));
            editData.cloudVariationEnabled = fabsf(editData.cloudVariation - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.cloudVariation = true;
            editChanged = true;
        } else {
            fabsf(cloudVariation - 1.0f) <= 0.001f ? g_oCloudVariation.clear() : g_oCloudVariation.set(cloudVariation);
        }
    }

    if (!enabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::ExperimentControls);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Legacy Fog");
    float fogPct = detachedEdit ? editData.fogPercent : 0.0f;
    if (!detachedEdit && g_oFog.active.load()) {
        const float fogN = sqrtf(min(1.0f, max(0.0f, g_oFog.value.load() / 100.0f)));
        fogPct = fogN * 100.0f;
    }

    const bool fogBlocked = detachedEdit ? (editData.forceClearSky || editData.noFog) : (g_forceClear.load() || g_noFog.load());
    const bool fogFeatureAvailable = !fogBlocked && RuntimeFeatureAvailable(RuntimeFeatureId::FogControls);
    if (!fogFeatureAvailable) {
        ImGui::BeginDisabled();
    }
    const bool fogNative = detachedEdit ? !editData.fogEnabled : !g_oFog.active.load();
    bool fogChanged = false;
    bool fogOverrideChanged = false;
    const bool fogReset = DrawSliderFloatRow("Fog [LEGACY]", "fog_legacy", &fogPct, 0.0f, 100.0f, "%.1f%%", &fogChanged, regionScoped ? &overrideMask.fog : nullptr, &fogOverrideChanged, fogNative);

    if (fogReset) {
        if (detachedEdit) {
            editData.fogEnabled = false;
            editData.fogPercent = 0.0f;
            if (regionScoped) overrideMask.fog = true;
            editChanged = true;
        } else {
            g_oFog.clear();
        }
    } else if (fogOverrideChanged) {
        editChanged = true;
    } else if (fogFeatureAvailable && fogChanged) {
        if (detachedEdit) {
            editData.fogPercent = min(100.0f, max(0.0f, fogPct));
            editData.fogEnabled = editData.fogPercent > 0.001f;
            if (regionScoped) overrideMask.fog = true;
            editChanged = true;
        } else {
            const float t = fogPct * 0.01f;
            g_oFog.set(t * t * 100.0f);
        }
    }

    if (!fogFeatureAvailable) {
        ImGui::EndDisabled();
        if (!fogBlocked) {
            DrawFeatureUnavailable(RuntimeFeatureId::FogControls);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Details");
    float puddleScale = detachedEdit ? editData.puddleScale : (g_oCloudThk.active.load() ? g_oCloudThk.value.load() : 0.0f);
    const bool puddleScaleNative = detachedEdit ? !editData.puddleScaleEnabled : !g_oCloudThk.active.load();
    const bool detailEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::DetailControls);
    if (!detailEnabled) {
        ImGui::BeginDisabled();
    }
    bool puddleScaleChanged = false;
    bool puddleScaleOverrideChanged = false;
    if (DrawSliderFloatRow("Puddle Scale", "puddle", &puddleScale, 0.0f, 1.0f, "%.3f", &puddleScaleChanged, regionScoped ? &overrideMask.puddleScale : nullptr, &puddleScaleOverrideChanged, puddleScaleNative)) {
        if (detachedEdit) {
            editData.puddleScaleEnabled = false;
            editData.puddleScale = 0.0f;
            if (regionScoped) overrideMask.puddleScale = true;
            editChanged = true;
        } else {
            g_oCloudThk.clear();
        }
    } else if (puddleScaleOverrideChanged) {
        editChanged = true;
    } else if (detailEnabled && puddleScaleChanged) {
        if (detachedEdit) {
            editData.puddleScale = min(1.0f, max(0.0f, puddleScale));
            editData.puddleScaleEnabled = editData.puddleScale > 0.001f;
            if (regionScoped) overrideMask.puddleScale = true;
            editChanged = true;
        } else {
            g_oCloudThk.set(puddleScale);
        }
    }
    if (!detailEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::DetailControls);
    }

    if (detachedEdit && editChanged) {
        if (regionScoped) {
            Preset_SetEditRegionDataWithOverrides(editData, overrideMask);
        } else {
            Preset_SetEditRegionData(editData);
        }
    }
}

void DrawCelestialTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData editData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask overrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    bool editChanged = false;

    ImGui::SeparatorText("Night Sky");
    const bool celestialEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::CelestialControls);
    if (!celestialEnabled) {
        ImGui::BeginDisabled();
    }

    const float nativeNightSkyPitch = (g_windPackBase0AValid.load() && g_windPackBase0BValid.load())
        ? min(89.0f, max(-89.0f, g_windPackBase0A.load() + 90.0f - g_windPackBase0B.load()))
        : 0.0f;
    const float nativeNightSkyYaw = g_sceneBaseNightSkyYaw.load();
    const float nativeSunSize = g_atmoBaseSunSize.load();
    const float nativeSunYaw = g_sceneBaseSunYaw.load();
    const float nativeSunPitch = g_sceneBaseSunPitch.load();
    const float nativeMoonSize = g_atmoBaseMoonSize.load();
    const float nativeMoonYaw = g_sceneBaseMoonYaw.load();
    const float nativeMoonPitch = g_sceneBaseMoonPitch.load();
    const float nativeMoonRoll = 0.0f;

    float nightSkyPitch = detachedEdit
        ? (editData.nightSkyRotationEnabled ? editData.nightSkyRotation : nativeNightSkyPitch)
        : (g_oExpNightSkyRot.active.load() ? g_oExpNightSkyRot.value.load() : nativeNightSkyPitch);
    const bool nightSkyPitchNative = detachedEdit ? !editData.nightSkyRotationEnabled : !g_oExpNightSkyRot.active.load();
    bool nightSkyPitchChanged = false;
    bool nightSkyPitchOverrideChanged = false;
    if (DrawSliderFloatRow("Night Sky Tilt", "night_sky_tilt", &nightSkyPitch, -89.0f, 89.0f, "%.2f", &nightSkyPitchChanged, regionScoped ? &overrideMask.nightSkyRotation : nullptr, &nightSkyPitchOverrideChanged, nightSkyPitchNative)) {
        if (detachedEdit) {
            editData.nightSkyRotationEnabled = false;
            editData.nightSkyRotation = nativeNightSkyPitch;
            if (regionScoped) overrideMask.nightSkyRotation = true;
            editChanged = true;
        } else {
            g_oExpNightSkyRot.clear();
        }
    } else if (nightSkyPitchOverrideChanged) {
        editChanged = true;
    } else if (celestialEnabled && nightSkyPitchChanged) {
        if (detachedEdit) {
            editData.nightSkyRotation = min(89.0f, max(-89.0f, nightSkyPitch));
            editData.nightSkyRotationEnabled = fabsf(editData.nightSkyRotation - nativeNightSkyPitch) > 0.001f;
            if (regionScoped) overrideMask.nightSkyRotation = true;
            editChanged = true;
        } else {
            nightSkyPitch = min(89.0f, max(-89.0f, nightSkyPitch));
            fabsf(nightSkyPitch - nativeNightSkyPitch) <= 0.001f ? g_oExpNightSkyRot.clear() : g_oExpNightSkyRot.set(nightSkyPitch);
        }
    }

    float nightSkyYaw = detachedEdit
        ? (editData.nightSkyYawEnabled ? editData.nightSkyYaw : nativeNightSkyYaw)
        : (g_oNightSkyYaw.active.load() ? g_oNightSkyYaw.value.load() : nativeNightSkyYaw);
    const bool nightSkyYawNative = detachedEdit ? !editData.nightSkyYawEnabled : !g_oNightSkyYaw.active.load();
    bool nightSkyYawChanged = false;
    bool nightSkyYawOverrideChanged = false;
    if (DrawSliderFloatRow("Night Sky Phase", "night_sky_phase", &nightSkyYaw, -180.0f, 180.0f, "%.2f", &nightSkyYawChanged, regionScoped ? &overrideMask.nightSkyYaw : nullptr, &nightSkyYawOverrideChanged, nightSkyYawNative)) {
        if (detachedEdit) {
            editData.nightSkyYawEnabled = false;
            editData.nightSkyYaw = nativeNightSkyYaw;
            if (regionScoped) overrideMask.nightSkyYaw = true;
            editChanged = true;
        } else {
            g_oNightSkyYaw.clear();
        }
    } else if (nightSkyYawOverrideChanged) {
        editChanged = true;
    } else if (celestialEnabled && nightSkyYawChanged) {
        if (detachedEdit) {
            editData.nightSkyYaw = min(180.0f, max(-180.0f, nightSkyYaw));
            editData.nightSkyYawEnabled = fabsf(editData.nightSkyYaw - nativeNightSkyYaw) > 0.001f;
            if (regionScoped) overrideMask.nightSkyYaw = true;
            editChanged = true;
        } else {
            nightSkyYaw = min(180.0f, max(-180.0f, nightSkyYaw));
            fabsf(nightSkyYaw - nativeNightSkyYaw) <= 0.001f ? g_oNightSkyYaw.clear() : g_oNightSkyYaw.set(nightSkyYaw);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Sun");

    float sunSize = detachedEdit
        ? (editData.sunSizeEnabled ? editData.sunSize : nativeSunSize)
        : (g_oSunSize.active.load() ? g_oSunSize.value.load() : nativeSunSize);
    const bool sunSizeNative = detachedEdit ? !editData.sunSizeEnabled : !g_oSunSize.active.load();
    bool sunSizeChanged = false;
    bool sunSizeOverrideChanged = false;
    if (DrawSliderFloatRow("Sun Size", "sun_size", &sunSize, 0.01f, 10.0f, "%.3f", &sunSizeChanged, regionScoped ? &overrideMask.sunSize : nullptr, &sunSizeOverrideChanged, sunSizeNative)) {
        if (detachedEdit) {
            editData.sunSizeEnabled = false;
            editData.sunSize = nativeSunSize;
            if (regionScoped) overrideMask.sunSize = true;
            editChanged = true;
        } else {
            g_oSunSize.clear();
        }
    } else if (sunSizeOverrideChanged) {
        editChanged = true;
    } else if (celestialEnabled && sunSizeChanged) {
        if (detachedEdit) {
            editData.sunSize = min(10.0f, max(0.01f, sunSize));
            editData.sunSizeEnabled = fabsf(editData.sunSize - nativeSunSize) > 0.001f;
            if (regionScoped) overrideMask.sunSize = true;
            editChanged = true;
        } else {
            sunSize = min(10.0f, max(0.01f, sunSize));
            fabsf(sunSize - nativeSunSize) <= 0.001f ? g_oSunSize.clear() : g_oSunSize.set(sunSize);
        }
    }

    float sunYaw = detachedEdit
        ? (editData.sunYawEnabled ? editData.sunYaw : nativeSunYaw)
        : (g_oSunDirX.active.load() ? g_oSunDirX.value.load() : nativeSunYaw);
    const bool sunYawNative = detachedEdit ? !editData.sunYawEnabled : !g_oSunDirX.active.load();
    bool sunYawChanged = false;
    bool sunYawOverrideChanged = false;
    if (DrawSliderFloatRow("Sun Yaw Lock", "sun_yaw", &sunYaw, -180.0f, 180.0f, "%.2f", &sunYawChanged, regionScoped ? &overrideMask.sunYaw : nullptr, &sunYawOverrideChanged, sunYawNative)) {
        if (detachedEdit) {
            editData.sunYawEnabled = false;
            editData.sunYaw = nativeSunYaw;
            if (regionScoped) overrideMask.sunYaw = true;
            editChanged = true;
        } else {
            g_oSunDirX.clear();
        }
    } else if (sunYawOverrideChanged) {
        editChanged = true;
    } else if (celestialEnabled && sunYawChanged) {
        if (detachedEdit) {
            editData.sunYaw = min(180.0f, max(-180.0f, sunYaw));
            editData.sunYawEnabled = fabsf(editData.sunYaw - nativeSunYaw) > 0.001f;
            if (regionScoped) overrideMask.sunYaw = true;
            editChanged = true;
        } else {
            sunYaw = min(180.0f, max(-180.0f, sunYaw));
            fabsf(sunYaw - nativeSunYaw) <= 0.001f ? g_oSunDirX.clear() : g_oSunDirX.set(sunYaw);
        }
    }

    float sunPitch = detachedEdit
        ? (editData.sunPitchEnabled ? editData.sunPitch : nativeSunPitch)
        : (g_oSunDirY.active.load() ? g_oSunDirY.value.load() : nativeSunPitch);
    const bool sunPitchNative = detachedEdit ? !editData.sunPitchEnabled : !g_oSunDirY.active.load();
    bool sunPitchChanged = false;
    bool sunPitchOverrideChanged = false;
    if (DrawSliderFloatRow("Sun Pitch Lock", "sun_pitch", &sunPitch, -89.0f, 89.0f, "%.2f", &sunPitchChanged, regionScoped ? &overrideMask.sunPitch : nullptr, &sunPitchOverrideChanged, sunPitchNative)) {
        if (detachedEdit) {
            editData.sunPitchEnabled = false;
            editData.sunPitch = nativeSunPitch;
            if (regionScoped) overrideMask.sunPitch = true;
            editChanged = true;
        } else {
            g_oSunDirY.clear();
        }
    } else if (sunPitchOverrideChanged) {
        editChanged = true;
    } else if (celestialEnabled && sunPitchChanged) {
        if (detachedEdit) {
            editData.sunPitch = min(89.0f, max(-89.0f, sunPitch));
            editData.sunPitchEnabled = fabsf(editData.sunPitch - nativeSunPitch) > 0.001f;
            if (regionScoped) overrideMask.sunPitch = true;
            editChanged = true;
        } else {
            sunPitch = min(89.0f, max(-89.0f, sunPitch));
            fabsf(sunPitch - nativeSunPitch) <= 0.001f ? g_oSunDirY.clear() : g_oSunDirY.set(sunPitch);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Moon");
    int moonTexture = detachedEdit
        ? (editData.moonTextureEnabled ? MoonTextureFindOptionByName(editData.moonTexture.c_str()) : 0)
        : MoonTextureSelectedOption();
    if (moonTexture < 0) {
        moonTexture = 0;
    }
    const bool moonTextureReady = MoonTextureReady();
    const bool moonTextureOverrideChanged = DrawOverrideToggle(regionScoped ? &overrideMask.moonTexture : nullptr);
    const bool moonTextureRegionOverride = !regionScoped || overrideMask.moonTexture;
    if (!moonTextureRegionOverride) {
        ImGui::BeginDisabled();
    }
    ImGui::TextUnformatted("Moon Texture");
    if (!moonTextureRegionOverride) {
        ImGui::EndDisabled();
    }
    if (regionScoped) {
        DrawOverrideBadge(overrideMask.moonTexture);
    }
    ImGui::SameLine();
    const float refreshWidth = 64.0f;
    const float refreshX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - refreshWidth;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), refreshX));
    if (ImGui::Button("Refresh", ImVec2(refreshWidth, 0.0f))) {
        MoonTextureRefreshList();
    }
    char inheritedMoonTexture[192] = {};
    const char* moonTexturePreview = MoonTextureOptionName(moonTexture);
    if (!moonTextureRegionOverride) {
        sprintf_s(inheritedMoonTexture, sizeof(inheritedMoonTexture), "G: %s", moonTexturePreview);
        moonTexturePreview = inheritedMoonTexture;
    }
    ImGui::TextDisabled("Selected: %s", moonTexturePreview);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##moon_texture_filter", "Search moon textures", g_moonTextureFilter, IM_ARRAYSIZE(g_moonTextureFilter));
    if (!moonTextureReady || !moonTextureRegionOverride) {
        ImGui::BeginDisabled();
    }
    const float moonListHeight = min(220.0f, max(112.0f, ImGui::GetTextLineHeightWithSpacing() * 9.0f));
    if (ImGui::BeginChild("MoonTextureLibrary", ImVec2(0.0f, moonListHeight), true)) {
        const int optionCount = MoonTextureOptionCount();
        const char* currentPack = nullptr;
        int visibleCount = 0;
        for (int i = 0; i < optionCount; ++i) {
            const char* optionName = MoonTextureOptionName(i);
            const char* optionLabel = MoonTextureOptionLabel(i);
            const bool visible = i == 0
                ? TextContainsNoCase("Native", g_moonTextureFilter)
                : (TextContainsNoCase(optionLabel, g_moonTextureFilter) ||
                   TextContainsNoCase(optionName, g_moonTextureFilter) ||
                   TextContainsNoCase(MoonTextureOptionPack(i), g_moonTextureFilter));
            if (!visible) {
                continue;
            }
            if (i > 0) {
                const char* pack = MoonTextureOptionPack(i);
                const char* group = (pack && pack[0]) ? pack : "Loose";
                if (!currentPack || strcmp(currentPack, group) != 0) {
                    if (visibleCount > 0) {
                        ImGui::Separator();
                    }
                    ImGui::TextDisabled("%s", group);
                    currentPack = group;
                }
            }
            ++visibleCount;
            const bool selected = i == moonTexture;
            if (ImGui::Selectable(optionLabel, selected)) {
                moonTexture = i;
                if (detachedEdit) {
                    editData.moonTextureEnabled = i > 0;
                    editData.moonTexture = i > 0 ? optionName : "";
                    if (regionScoped) overrideMask.moonTexture = true;
                    editChanged = true;
                } else {
                    MoonTextureSelectOption(i);
                }
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        if (visibleCount == 0) {
            ImGui::TextDisabled("No moon texture matches");
        }
    }
    ImGui::EndChild();
    if (!moonTextureReady || !moonTextureRegionOverride) {
        ImGui::EndDisabled();
    }
    if (moonTextureOverrideChanged) {
        editChanged = true;
    }

    float moonSize = detachedEdit
        ? (editData.moonSizeEnabled ? editData.moonSize : nativeMoonSize)
        : (g_oMoonSize.active.load() ? g_oMoonSize.value.load() : nativeMoonSize);
    const bool moonSizeNative = detachedEdit ? !editData.moonSizeEnabled : !g_oMoonSize.active.load();
    bool moonSizeChanged = false;
    bool moonSizeOverrideChanged = false;
    if (DrawSliderFloatRow("Moon Size", "moon_size", &moonSize, 0.020f, 20.0f, "%.3f", &moonSizeChanged, regionScoped ? &overrideMask.moonSize : nullptr, &moonSizeOverrideChanged, moonSizeNative)) {
        if (detachedEdit) {
            editData.moonSizeEnabled = false;
            editData.moonSize = nativeMoonSize;
            if (regionScoped) overrideMask.moonSize = true;
            editChanged = true;
        } else {
            g_oMoonSize.clear();
        }
    } else if (moonSizeOverrideChanged) {
        editChanged = true;
    } else if (celestialEnabled && moonSizeChanged) {
        if (detachedEdit) {
            editData.moonSize = min(20.0f, max(0.020f, moonSize));
            editData.moonSizeEnabled = fabsf(editData.moonSize - nativeMoonSize) > 0.001f;
            if (regionScoped) overrideMask.moonSize = true;
            editChanged = true;
        } else {
            moonSize = min(20.0f, max(0.020f, moonSize));
            fabsf(moonSize - nativeMoonSize) <= 0.001f ? g_oMoonSize.clear() : g_oMoonSize.set(moonSize);
        }
    }

    float moonYaw = detachedEdit
        ? (editData.moonYawEnabled ? editData.moonYaw : nativeMoonYaw)
        : (g_oMoonDirX.active.load() ? g_oMoonDirX.value.load() : nativeMoonYaw);
    const bool moonYawNative = detachedEdit ? !editData.moonYawEnabled : !g_oMoonDirX.active.load();
    bool moonYawChanged = false;
    bool moonYawOverrideChanged = false;
    if (DrawSliderFloatRow("Moon Yaw Lock", "moon_yaw", &moonYaw, -180.0f, 180.0f, "%.2f", &moonYawChanged, regionScoped ? &overrideMask.moonYaw : nullptr, &moonYawOverrideChanged, moonYawNative)) {
        if (detachedEdit) {
            editData.moonYawEnabled = false;
            editData.moonYaw = nativeMoonYaw;
            if (regionScoped) overrideMask.moonYaw = true;
            editChanged = true;
        } else {
            g_oMoonDirX.clear();
        }
    } else if (moonYawOverrideChanged) {
        editChanged = true;
    } else if (celestialEnabled && moonYawChanged) {
        if (detachedEdit) {
            editData.moonYaw = min(180.0f, max(-180.0f, moonYaw));
            editData.moonYawEnabled = fabsf(editData.moonYaw - nativeMoonYaw) > 0.001f;
            if (regionScoped) overrideMask.moonYaw = true;
            editChanged = true;
        } else {
            moonYaw = min(180.0f, max(-180.0f, moonYaw));
            fabsf(moonYaw - nativeMoonYaw) <= 0.001f ? g_oMoonDirX.clear() : g_oMoonDirX.set(moonYaw);
        }
    }

    float moonPitch = detachedEdit
        ? (editData.moonPitchEnabled ? editData.moonPitch : nativeMoonPitch)
        : (g_oMoonDirY.active.load() ? g_oMoonDirY.value.load() : nativeMoonPitch);
    const bool moonPitchNative = detachedEdit ? !editData.moonPitchEnabled : !g_oMoonDirY.active.load();
    bool moonPitchChanged = false;
    bool moonPitchOverrideChanged = false;
    if (DrawSliderFloatRow("Moon Pitch Lock", "moon_pitch", &moonPitch, -89.0f, 89.0f, "%.2f", &moonPitchChanged, regionScoped ? &overrideMask.moonPitch : nullptr, &moonPitchOverrideChanged, moonPitchNative)) {
        if (detachedEdit) {
            editData.moonPitchEnabled = false;
            editData.moonPitch = nativeMoonPitch;
            if (regionScoped) overrideMask.moonPitch = true;
            editChanged = true;
        } else {
            g_oMoonDirY.clear();
        }
    } else if (moonPitchOverrideChanged) {
        editChanged = true;
    } else if (celestialEnabled && moonPitchChanged) {
        if (detachedEdit) {
            editData.moonPitch = min(89.0f, max(-89.0f, moonPitch));
            editData.moonPitchEnabled = fabsf(editData.moonPitch - nativeMoonPitch) > 0.001f;
            if (regionScoped) overrideMask.moonPitch = true;
            editChanged = true;
        } else {
            moonPitch = min(89.0f, max(-89.0f, moonPitch));
            fabsf(moonPitch - nativeMoonPitch) <= 0.001f ? g_oMoonDirY.clear() : g_oMoonDirY.set(moonPitch);
        }
    }

    float moonRoll = detachedEdit
        ? (editData.moonRollEnabled ? editData.moonRoll : nativeMoonRoll)
        : (g_oMoonRoll.active.load() ? g_oMoonRoll.value.load() : nativeMoonRoll);
    const bool moonRollNative = detachedEdit ? !editData.moonRollEnabled : !g_oMoonRoll.active.load();
    bool moonRollChanged = false;
    bool moonRollOverrideChanged = false;
    if (DrawSliderFloatRow("Moon Rotation", "moon_roll", &moonRoll, -180.0f, 180.0f, "%.2f", &moonRollChanged, regionScoped ? &overrideMask.moonRoll : nullptr, &moonRollOverrideChanged, moonRollNative)) {
        if (detachedEdit) {
            editData.moonRollEnabled = false;
            editData.moonRoll = nativeMoonRoll;
            if (regionScoped) overrideMask.moonRoll = true;
            editChanged = true;
        } else {
            g_oMoonRoll.clear();
        }
    } else if (moonRollOverrideChanged) {
        editChanged = true;
    } else if (celestialEnabled && moonRollChanged) {
        if (detachedEdit) {
            editData.moonRoll = min(180.0f, max(-180.0f, moonRoll));
            editData.moonRollEnabled = fabsf(editData.moonRoll - nativeMoonRoll) > 0.001f;
            if (regionScoped) overrideMask.moonRoll = true;
            editChanged = true;
        } else {
            moonRoll = min(180.0f, max(-180.0f, moonRoll));
            fabsf(moonRoll - nativeMoonRoll) <= 0.001f ? g_oMoonRoll.clear() : g_oMoonRoll.set(moonRoll);
        }
    }

    if (!celestialEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::CelestialControls);
    }

    if (detachedEdit && editChanged) {
        if (regionScoped) {
            Preset_SetEditRegionDataWithOverrides(editData, overrideMask);
        } else {
            Preset_SetEditRegionData(editData);
        }
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

    const char* nexusLabel = "nexusmods";
    const float nexusWidth = ImGui::CalcTextSize(nexusLabel).x;
    const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), rightEdge - nexusWidth));
    ImGui::TextLinkOpenURL(nexusLabel, "https://www.nexusmods.com/crimsondesert/mods/632");

    ImGui::Text("%s %s", MOD_NAME, MOD_VERSION);
    ImGui::SameLine();
    DrawEditScopeCombo();
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
        if (ImGui::BeginTabItem("Celestial")) {
            DrawCelestialTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Experiment")) {
            DrawExperimentTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Status")) {
            DrawStatusTab();
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
