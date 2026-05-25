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
std::string g_sliderEditId;
bool g_sliderEditFocusRequest = false;
const char* NumericInputFormat(const char* format, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return "%.3f";
    }
    strcpy_s(out, outSize, "%.3f");
    if (!format || !format[0]) {
        return out;
    }

    const char* percent = strchr(format, '%');
    while (percent && percent[1] == '%') {
        percent = strchr(percent + 2, '%');
    }
    if (!percent) {
        return out;
    }

    size_t len = 1;
    while (percent[len]) {
        const char c = percent[len++];
        if (c == 'f' || c == 'F' || c == 'e' || c == 'E' || c == 'g' || c == 'G') {
            break;
        }
    }
    len = min(len, outSize - 1);
    memcpy(out, percent, len);
    out[len] = '\0';
    return out;
}

int HourToMinuteOfDay(float hour) {
    const float normalized = NormalizeHour24(hour);
    int minutes = static_cast<int>(std::lround(normalized * 60.0f));
    minutes %= 24 * 60;
    if (minutes < 0) {
        minutes += 24 * 60;
    }
    return minutes;
}

int HourToMinuteOfDayFloor(float hour) {
    const float normalized = NormalizeHour24(hour);
    int minutes = static_cast<int>(std::floor(normalized * 60.0f + 0.0001f));
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

void FormatGameClockFromMinute(int minuteOfDay, char* out, size_t outSize) {
    minuteOfDay %= 24 * 60;
    if (minuteOfDay < 0) {
        minuteOfDay += 24 * 60;
    }
    const int rawHour = minuteOfDay / 60;
    const int minute = minuteOfDay % 60;
    const int displayHour = rawHour <= 12 ? rawHour : rawHour - 12;
    const char* meridiem = rawHour > 11 ? "PM" : "AM";
    sprintf_s(out, outSize, "%d:%02d %s", displayHour, minute, meridiem);
}

void FormatGameClockFromHour(float hour, char* out, size_t outSize) {
    FormatGameClockFromMinute(HourToMinuteOfDay(hour), out, outSize);
}

bool TryGetHudGameTimeHour(float* outHour) {
    if (!outHour || !g_timeUiClockSourceValid.load() || !g_timeUiClockValid.load()) {
        return false;
    }

    const int hour = g_timeUiClockHour24.load();
    const int minute = g_timeUiClockMinute.load();
    if (hour < 0 || hour >= 24 || minute < 0 || minute >= 60) {
        return false;
    }

    *outHour = MinuteOfDayToHour(hour * 60 + minute);
    return true;
}

float ClampProgressVisualTimeIntervalMs(float value) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return min(5000.0f, max(0.0f, value));
}

void FormatProgressVisualTimeInterval(float value, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    const float clamped = ClampProgressVisualTimeIntervalMs(value);
    if (clamped <= 0.5f) {
        strcpy_s(out, outSize, "Every frame");
        return;
    }
    sprintf_s(out, outSize, "%.0f ms", clamped);
}

SliderRange ActiveSliderRange(float normalLo, float normalHi, float extendedLo, float extendedHi) {
    return g_extendedSliderRange.load()
        ? SliderRange{ extendedLo, extendedHi }
        : SliderRange{ normalLo, normalHi };
}

float ClampSliderValue(float value, const SliderRange& range) {
    if (!std::isfinite(value)) {
        return range.lo;
    }
    return min(range.hi, max(range.lo, value));
}

float ClampColorValue(float value) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return min(10.0f, max(0.0f, value));
}

void ClampColorValues(float* color, bool includeAlpha) {
    if (!color) return;
    color[0] = ClampColorValue(color[0]);
    color[1] = ClampColorValue(color[1]);
    color[2] = ClampColorValue(color[2]);
    if (includeAlpha) {
        color[3] = ClampColorValue(color[3]);
    } else {
        color[3] = 1.0f;
    }
}

WeatherPresetColor ColorFromArray(const float* color, bool includeAlpha) {
    return {
        color ? color[0] : 1.0f,
        color ? color[1] : 1.0f,
        color ? color[2] : 1.0f,
        includeAlpha && color ? color[3] : 1.0f,
    };
}

unsigned int PackUiRgbBits(float r, float g, float b) {
    const auto toByte = [](float value) -> unsigned int {
        return static_cast<unsigned int>(min(1.0f, max(0.0f, value)) * 255.0f + 0.5f);
    };
    return (toByte(r) << 16) | (toByte(g) << 8) | toByte(b);
}

WeatherPresetColor RayleighColorFromUiBits(unsigned int bits) {
    return {
        static_cast<float>((bits >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((bits >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(bits & 0xFFu) / 255.0f,
        1.0f,
    };
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

void DrawFeatureUnavailable(RuntimeFeatureId feature) {
    if (RuntimeFeatureAvailable(feature)) {
        return;
    }
    const char* note = RuntimeFeatureNote(feature);
    if (note && note[0]) {
        ImGui::TextDisabled("%s", note);
    }
}

bool HookReady(RuntimeHookId hook) {
    return RuntimeHookEnabled(hook);
}

void DrawHookUnavailable(RuntimeHookId hook) {
    ImGui::TextDisabled("Required hook disabled: %s", RuntimeHookLabel(hook));
}

bool WeatherTickReady() { return HookReady(RuntimeHookId::WeatherTick); }
bool RainHookReady() { return HookReady(RuntimeHookId::GetRainIntensity); }
bool SnowHookReady() { return HookReady(RuntimeHookId::GetSnowIntensity); }
bool DustHookReady() { return HookReady(RuntimeHookId::GetDustIntensity); }
bool WindPackReady() { return HookReady(RuntimeHookId::WindPack); }
bool SceneFrameReady() { return HookReady(RuntimeHookId::SceneFrameUpdate); }
bool WeatherFrameReady() { return HookReady(RuntimeHookId::WeatherFrameUpdate); }
bool RealGameTimeReady() { return HookReady(RuntimeHookId::GameTimeUpdate) && HookReady(RuntimeHookId::GameTimeGetter); }

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

void DrawTexturePackHeader(const char* packName) {
    const char* pack = (packName && packName[0]) ? packName : "Loose";
    char header[192] = {};
    sprintf_s(header, sizeof(header), "%s", pack);

    ImGui::Spacing();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = ImGui::GetTextLineHeightWithSpacing() + 2.0f;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 bg = ImGui::GetColorU32(ImVec4(0.13f, 0.17f, 0.22f, 1.0f));
    const ImU32 accent = ImGui::GetColorU32(ImVec4(0.34f, 0.52f, 0.78f, 1.0f));
    const ImU32 text = ImGui::GetColorU32(ImVec4(0.66f, 0.76f, 0.88f, 1.0f));

    drawList->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), bg);
    drawList->AddRectFilled(pos, ImVec2(pos.x + 3.0f, pos.y + height), accent);
    drawList->AddText(ImVec2(pos.x + 8.0f, pos.y + 2.0f), text, header);
    ImGui::Dummy(ImVec2(width, height));
}

bool DrawSliderFloatRow(
    const char* label,
    const char* id,
    float* value,
    float minValue,
    float maxValue,
    const char* format,
    bool* outValueChanged,
    bool* overrideEnabled,
    bool* outOverrideChanged,
    bool nativeDisplay,
    bool centerOnNative,
    float nativeValue) {
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
    char centeredFormat[64] = {};
    const char* displayFormat = format;
    if (nativeDisplay) {
        displayFormat = "NATIVE";
    } else if (centerOnNative) {
        sprintf_s(centeredFormat, sizeof(centeredFormat), format, *value);
        displayFormat = centeredFormat;
    }
    if (sliderDisabled) {
        sprintf_s(inheritedFormat, sizeof(inheritedFormat), "G: %s", nativeDisplay ? "NATIVE" : format);
        displayFormat = inheritedFormat;
        ImGui::BeginDisabled();
    }
    bool valueChanged = false;
    const bool editing = g_sliderEditId == id;
    if (editing) {
        char inputFormat[24] = {};
        if (g_sliderEditFocusRequest) {
            ImGui::SetKeyboardFocusHere();
            g_sliderEditFocusRequest = false;
        }
        valueChanged = ImGui::InputFloat(
            "##value",
            value,
            0.0f,
            0.0f,
            NumericInputFormat(format, inputFormat, sizeof(inputFormat)),
            ImGuiInputTextFlags_AutoSelectAll);
        if (valueChanged) {
            *value = min(maxValue, max(minValue, *value));
        }

        const bool finishEdit = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                                ImGui::IsKeyPressed(ImGuiKey_Escape) ||
                                ImGui::IsItemDeactivated();
        if (finishEdit) {
            g_sliderEditId.clear();
            g_sliderEditFocusRequest = false;
        }
    } else {
        if (centerOnNative && nativeValue > minValue && nativeValue < maxValue) {
            float sliderPosition = *value <= nativeValue
                ? (*value - nativeValue) / (nativeValue - minValue)
                : (*value - nativeValue) / (maxValue - nativeValue);
            if (ImGui::SliderFloat("##value", &sliderPosition, -1.0f, 1.0f, displayFormat)) {
                *value = sliderPosition <= 0.0f
                    ? nativeValue + sliderPosition * (nativeValue - minValue)
                    : nativeValue + sliderPosition * (maxValue - nativeValue);
                valueChanged = true;
            }
        } else {
            valueChanged = ImGui::SliderFloat("##value", value, minValue, maxValue, displayFormat);
        }
        if (!sliderDisabled && ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            g_sliderEditId = id;
            g_sliderEditFocusRequest = true;
        }
    }
    if (sliderDisabled) {
        ImGui::EndDisabled();
    }
    if (reset && g_sliderEditId == id) {
        g_sliderEditId.clear();
        g_sliderEditFocusRequest = false;
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

bool DrawColorRow(
    const char* label,
    const char* id,
    float* color,
    bool includeAlpha,
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
    const bool disabled = overrideEnabled && !*overrideEnabled;
    if (disabled) {
        ImGui::BeginDisabled();
    }
    const bool reset = ImGui::Button("R", ImVec2(resetWidth, 0.0f));
    if (disabled) {
        ImGui::EndDisabled();
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (disabled) {
        ImGui::BeginDisabled();
    }
    bool valueChanged = includeAlpha
        ? ImGui::ColorEdit4("##color", color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)
        : ImGui::ColorEdit3("##color", color, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
    if (disabled) {
        ImGui::EndDisabled();
    }
    if (nativeDisplay) {
        ImGui::SameLine();
        ImGui::TextDisabled("NATIVE");
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

bool DrawClockDial(const char* id, int* minuteOfDay, bool centerDial) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = kPi * 2.0f;
    constexpr float kDialSize = 116.0f;

    ImGui::PushID(id);
    if (centerDial) {
        const float availWidth = ImGui::GetContentRegionAvail().x;
        const float centeredX = ImGui::GetCursorPosX() + max(0.0f, (availWidth - kDialSize) * 0.5f);
        ImGui::SetCursorPosX(centeredX);
    }
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
bool IsSliderTextEditActive(const char* id) {
    return id && g_sliderEditId == id;
}

bool ConsumeSliderTextEditFocusRequest() {
    if (!g_sliderEditFocusRequest) {
        return false;
    }
    g_sliderEditFocusRequest = false;
    return true;
}

void BeginSliderTextEdit(const char* id) {
    g_sliderEditId = id ? id : "";
    g_sliderEditFocusRequest = true;
}

void EndSliderTextEdit() {
    g_sliderEditId.clear();
    g_sliderEditFocusRequest = false;
}

} // namespace overlay_internal
