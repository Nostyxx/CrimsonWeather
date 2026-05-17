#include "pch.h"

#include "overlay_bridge.h"
#include "sky_texture_override.h"
#include "preset_service.h"
#include "runtime_shared.h"

#include <imgui.h>
#include <reshade.hpp>

#include <d3d12.h>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <string>

namespace {

constexpr float kCloudScatteringCoefficientMin = 0.00001f;

HMODULE g_overlayModule = nullptr;
bool g_overlayRegistered = false;
char g_newPresetName[128] = "NewPreset.ini";
char g_presetFilter[96] = "";
char g_moonTextureFilter[96] = "";
char g_milkywayTextureFilter[96] = "";
char g_timeEditText[32] = "";
int g_timeEditLastMinute = -1;
bool g_timeEditActive = false;
bool g_timeEditFocusRequest = false;
bool g_timeEditHadFocus = false;
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

struct SliderRange {
    float lo;
    float hi;
};

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

#if defined(CW_DEV_BUILD)
float DevAtmosphereLabDisplayValue(size_t index) {
    if (index >= kDevAtmosphereLabFieldCount) {
        return 0.0f;
    }
    return g_devAtmosphereLabActive[index].load()
        ? g_devAtmosphereLabValue[index].load()
        : g_devAtmosphereLabLast[index].load();
}

bool DevAtmosphereLabAnyActive(size_t baseIndex, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const size_t index = baseIndex + i;
        if (index < kDevAtmosphereLabFieldCount && g_devAtmosphereLabActive[index].load()) {
            return true;
        }
    }
    return false;
}

void DevAtmosphereLabSetValues(size_t baseIndex, const float* values, size_t count) {
    if (!values) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const size_t index = baseIndex + i;
        if (index >= kDevAtmosphereLabFieldCount) {
            return;
        }
        g_devAtmosphereLabValue[index].store(values[i]);
        g_devAtmosphereLabActive[index].store(true);
    }
}

void DevAtmosphereLabClearValues(size_t baseIndex, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const size_t index = baseIndex + i;
        if (index >= kDevAtmosphereLabFieldCount) {
            return;
        }
        g_devAtmosphereLabActive[index].store(false);
    }
}

uint32_t DevAtmosphereLabFloatBits(float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float DevAtmosphereLabBitsToFloat(uint32_t bits) {
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

void DevAtmosphereLabSetRawBits(size_t index, uint32_t bits) {
    if (index >= kDevAtmosphereLabFieldCount) {
        return;
    }
    g_devAtmosphereLabValue[index].store(DevAtmosphereLabBitsToFloat(bits));
    g_devAtmosphereLabActive[index].store(true);
}

void DrawDevAtmosphereScalarField(
    const char* label,
    size_t index,
    float minValue,
    float maxValue,
    const char* format = "%.3f") {
    if (index >= kDevAtmosphereLabFieldCount) {
        return;
    }

    ImGui::PushID(label);
    float value = DevAtmosphereLabDisplayValue(index);
    const float nativeValue = g_devAtmosphereLabLast[index].load();
    const bool active = g_devAtmosphereLabActive[index].load();
    ImGui::Text("%s %s", label, active ? "(OVERRIDE)" : "(native)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Use Native")) {
        g_devAtmosphereLabValue[index].store(nativeValue);
        g_devAtmosphereLabActive[index].store(true);
        GUI_SetStatus("Atmosphere Lab copied native scalar");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        g_devAtmosphereLabActive[index].store(false);
        GUI_SetStatus("Atmosphere Lab scalar cleared");
    }
    if (ImGui::SliderFloat("Value", &value, minValue, maxValue, format)) {
        g_devAtmosphereLabValue[index].store(value);
        g_devAtmosphereLabActive[index].store(true);
        GUI_SetStatus("Atmosphere Lab scalar override active");
    }
    ImGui::TextDisabled("Field 0x%02X %s | native %.6g", static_cast<unsigned int>(index), DevAtmosphereLabFieldName(index), nativeValue);
    ImGui::PopID();
}

void DrawDevAtmospherePackedRayleighColor() {
    constexpr size_t kRayleighField = 0x0F;
    uint32_t bits = DevAtmosphereLabFloatBits(g_devAtmosphereLabActive[kRayleighField].load()
        ? g_devAtmosphereLabValue[kRayleighField].load()
        : g_devAtmosphereLabLast[kRayleighField].load());
    uint32_t nativeBits = DevAtmosphereLabFloatBits(g_devAtmosphereLabLast[kRayleighField].load());

    float value[3] = {
        static_cast<float>((bits >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((bits >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(bits & 0xFFu) / 255.0f,
    };
    const float nativeValue[3] = {
        static_cast<float>((nativeBits >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((nativeBits >> 8) & 0xFFu) / 255.0f,
        static_cast<float>(nativeBits & 0xFFu) / 255.0f,
    };

    ImGui::PushID("RayleighPackedColor");
    ImGui::Text("Rayleigh Scattering Color 0x0F %s", g_devAtmosphereLabActive[kRayleighField].load() ? "(OVERRIDE)" : "(native)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Use Native")) {
        DevAtmosphereLabSetRawBits(kRayleighField, nativeBits);
        GUI_SetStatus("Atmosphere Lab copied native Rayleigh color");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        g_devAtmosphereLabActive[kRayleighField].store(false);
        GUI_SetStatus("Atmosphere Lab Rayleigh color cleared");
    }

    if (ImGui::ColorEdit3("RGB", value, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR)) {
        const auto r = static_cast<uint32_t>(min(1.0f, max(0.0f, value[0])) * 255.0f + 0.5f);
        const auto g = static_cast<uint32_t>(min(1.0f, max(0.0f, value[1])) * 255.0f + 0.5f);
        const auto b = static_cast<uint32_t>(min(1.0f, max(0.0f, value[2])) * 255.0f + 0.5f);
        DevAtmosphereLabSetRawBits(kRayleighField, (r << 16) | (g << 8) | b);
        GUI_SetStatus("Atmosphere Lab Rayleigh color override active");
    }
    ImGui::TextDisabled(
        "Native: R %.3f  G %.3f  B %.3f | bits 0x%08X",
        nativeValue[0],
        nativeValue[1],
        nativeValue[2],
        nativeBits);
    ImGui::PopID();
}

void DrawDevAtmosphereColorEditor(const char* label, size_t baseIndex, bool includeAlpha) {
    constexpr size_t kColorComponentCount = 4;
    float value[kColorComponentCount] = {
        DevAtmosphereLabDisplayValue(baseIndex + 0),
        DevAtmosphereLabDisplayValue(baseIndex + 1),
        DevAtmosphereLabDisplayValue(baseIndex + 2),
        DevAtmosphereLabDisplayValue(baseIndex + 3),
    };
    const float nativeValue[kColorComponentCount] = {
        g_devAtmosphereLabLast[baseIndex + 0].load(),
        g_devAtmosphereLabLast[baseIndex + 1].load(),
        g_devAtmosphereLabLast[baseIndex + 2].load(),
        g_devAtmosphereLabLast[baseIndex + 3].load(),
    };

    ImGui::PushID(label);
    const bool active = DevAtmosphereLabAnyActive(baseIndex, includeAlpha ? 4 : 3);
    ImGui::Text("%s %s", label, active ? "(OVERRIDE)" : "(native)");
    ImGui::SameLine();
    if (ImGui::SmallButton("Use Native")) {
        DevAtmosphereLabSetValues(baseIndex, nativeValue, includeAlpha ? 4 : 3);
        GUI_SetStatus("Atmosphere Lab copied native color");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) {
        DevAtmosphereLabClearValues(baseIndex, includeAlpha ? 4 : 3);
        GUI_SetStatus("Atmosphere Lab color cleared");
    }

    ImGuiColorEditFlags flags = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR;
    if (!includeAlpha) {
        flags |= ImGuiColorEditFlags_NoAlpha;
    }
    if (ImGui::ColorEdit4("RGBA", value, flags)) {
        DevAtmosphereLabSetValues(baseIndex, value, includeAlpha ? 4 : 3);
        GUI_SetStatus("Atmosphere Lab color override active");
    }

    ImGui::TextDisabled(
        "Native: R %.3f  G %.3f  B %.3f  A %.3f",
        nativeValue[0],
        nativeValue[1],
        nativeValue[2],
        nativeValue[3]);
    ImGui::PopID();
}

void DrawDevTab() {
    ImGui::TextDisabled("DEV-only live controls. These write WindPack atmosphere fields directly and are not saved to presets.");
    ImGui::SeparatorText("Atmosphere Color Lab");

    if (!WindPackReady()) {
        ImGui::BeginDisabled();
    }

    ImGui::Text("WindPack captures: %llu", g_devAtmosphereLabCaptureCount.load());
    DrawDevAtmospherePackedRayleighColor();
    ImGui::Spacing();
    DrawDevAtmosphereColorEditor("Volume Fog Scatter Color 0x34-0x37", 0x34, true);
    ImGui::Spacing();
    DrawDevAtmosphereColorEditor("Mie Scatter Color 0x38-0x3B", 0x38, true);

    ImGui::Spacing();
    ImGui::SeparatorText("Light Candidates");
    DrawDevAtmosphereScalarField("Sun Light Intensity", 0x00, 0.0f, 20.0f, "%.3f");
    DrawDevAtmosphereScalarField("Moon Light Intensity", 0x05, 0.0f, 20.0f, "%.3f");
    DrawDevAtmosphereScalarField("Directional Light Luminance Scale", 0x15, 0.0f, 20.0f, "%.3f");

    ImGui::Spacing();
    ImGui::SeparatorText("Fog Candidates");
    DrawDevAtmosphereScalarField("Mie Scaled Height", 0x10, 10.0f, 20000.0f, "%.1f");
    DrawDevAtmosphereScalarField("Mie Aerosol Density", 0x11, 0.0f, 20.0f, "%.4f");
    DrawDevAtmosphereScalarField("Mie Aerosol Absorption", 0x12, 0.0f, 5.0f, "%.4f");
    DrawDevAtmosphereScalarField("Mie Phase Const", 0x13, -0.99f, 0.99f, "%.4f");
    DrawDevAtmosphereScalarField("Height Fog Density", 0x17, 0.0f, 1.0f, "%.5f");
    DrawDevAtmosphereScalarField("Height Fog Baseline", 0x18, -5000.0f, 5000.0f, "%.1f");
    DrawDevAtmosphereScalarField("Height Fog Falloff", 0x19, 0.0f, 5.0f, "%.4f");
    DrawDevAtmosphereScalarField("Height Fog Scale", 0x1A, 0.0f, 10.0f, "%.4f");

    ImGui::Spacing();
    ImGui::SeparatorText("Cloud Candidates");
    DrawDevAtmosphereScalarField("Cloud Base Density", 0x1B, 0.0f, 5.0f, "%.4f");
    DrawDevAtmosphereScalarField("Cloud Alpha", 0x1E, 0.0f, 50.0f, "%.3f");
    DrawDevAtmosphereScalarField("Cloud Scattering Coefficient", 0x20, kCloudScatteringCoefficientMin, 1.0f, "%.5f");
    DrawDevAtmosphereScalarField("Cloud Phase Front", 0x21, -1.0f, 1.0f, "%.4f");
    DrawDevAtmosphereScalarField("Cloud Phase Back", 0x22, -1.0f, 1.0f, "%.4f");
    DrawDevAtmosphereScalarField("Cloud Cirrus Density", 0x2D, 0.0f, 5.0f, "%.4f");
    DrawDevAtmosphereScalarField("Cloud Cirrus Scale", 0x2E, 0.0f, 10.0f, "%.4f");
    DrawDevAtmosphereColorEditor("Cloud Cirrus Weight RGB 0x2F-0x31", 0x2F, false);

    ImGui::Spacing();
    if (ImGui::Button("Clear All Atmosphere Lab Overrides")) {
        DevAtmosphereLabClearValues(0, kDevAtmosphereLabFieldCount);
        GUI_SetStatus("Atmosphere Lab cleared");
    }

    if (!WindPackReady()) {
        ImGui::EndDisabled();
        DrawHookUnavailable(RuntimeHookId::WindPack);
    }
}
#endif

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
        valueChanged = ImGui::SliderFloat("##value", value, minValue, maxValue, displayFormat);
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
        } else if (strcmp(value, "HOOK DISABLED") == 0) {
            ImGui::SetTooltip("The required runtime hook is disabled in Hook Controls.");
        } else if (strcmp(value, "DISABLED") == 0) {
            ImGui::SetTooltip("This Crimson Weather override is disabled.");
        } else if (activeTooltip && activeTooltip[0]) {
            ImGui::SetTooltip("%s", activeTooltip);
        }
    }
    ImGui::TableNextColumn();
    if (strcmp(value, "HOOK DISABLED") == 0) {
        ImGui::TextDisabled("Hook Controls");
    } else {
        const std::string source = PresetStatusSourceLabel(snapshot, regionOverride);
        ImGui::TextUnformatted(source.c_str());
    }
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

void DrawStatusRowHookDisabled(
    const WeatherPresetStatusSnapshot& snapshot,
    const char* name,
    RuntimeHookId hook) {
    char tooltip[160] = {};
    sprintf_s(tooltip, sizeof(tooltip), "Required hook disabled: %s.", RuntimeHookLabel(hook));
    DrawStatusRowText(snapshot, name, "HOOK DISABLED", false, tooltip);
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

void DrawHookControlsTable() {
    RuntimeHookStatusEntry hooks[static_cast<size_t>(RuntimeHookId::Count)] = {};
    const size_t hookCount = GetRuntimeHookStatusEntries(hooks, _countof(hooks));

    ImGui::SeparatorText("Hook Controls");
    if (ImGui::BeginTable(
            "HookControlsTable",
            5,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Hook");
        ImGui::TableSetupColumn("Group");
        ImGui::TableSetupColumn("Target");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 82.0f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < hookCount; ++i) {
            const RuntimeHookStatusEntry& hook = hooks[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(hook.name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(hook.kind);
            ImGui::TableNextColumn();
            if (hook.target) {
                ImGui::Text("0x%p", hook.target);
            } else {
                ImGui::TextDisabled("Not found");
            }
            ImGui::TableNextColumn();
            if (!hook.installed) {
                ImGui::TextDisabled("Not installed");
            } else if (hook.enabled) {
                ImGui::TextColored(ImVec4(0.40f, 0.88f, 0.48f, 1.0f), "Enabled");
            } else {
                ImGui::TextColored(ImVec4(1.00f, 0.68f, 0.25f, 1.0f), "Disabled");
            }
            ImGui::TableNextColumn();
            if (!hook.installed) {
                ImGui::BeginDisabled();
            }
            ImGui::PushID(static_cast<int>(i));
            const char* label = hook.enabled ? "Disable" : "Enable";
            if (ImGui::Button(label, ImVec2(74.0f, 0.0f))) {
                char message[128] = {};
                if (SetRuntimeHookEnabled(hook.id, !hook.enabled, message, sizeof(message))) {
                    GUI_SetStatus(message);
                } else if (message[0]) {
                    GUI_SetStatus(message);
                } else {
                    GUI_SetStatus("Hook control failed");
                }
            }
            ImGui::PopID();
            if (!hook.installed) {
                ImGui::EndDisabled();
            }
        }

        ImGui::EndTable();
    }
}

void DrawStatusTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

    const WeatherPresetStatusSnapshot snapshot = Preset_GetStatusSnapshot();
    ImGui::Text("Active Preset: %s", Preset_GetSelectedDisplayName());
    ImGui::Text("Player Region: %s", Preset_GetRegionDisplayName(snapshot.playerRegion));
    ImGui::Text("Editing: %s", Preset_GetRegionDisplayName(snapshot.editRegion));
    bool extendedSliderRange = g_extendedSliderRange.load();
    if (ImGui::Checkbox("Extended Slider Range", &extendedSliderRange)) {
        g_extendedSliderRange.store(extendedSliderRange);
        SaveGeneralConfig();
        GUI_SetStatus(extendedSliderRange ? "Extended Slider Range enabled" : "Extended Slider Range disabled");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Allows sliders to go beyond recommended limits. Extreme values may cause visual glitches or rendering issues.");
    }
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
    DrawHookControlsTable();
    ImGui::Spacing();
    ImGui::SeparatorText("Preset Status");
    if (ImGui::BeginTable(
            "PresetStatusTable",
            3,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("Current");
        ImGui::TableSetupColumn("Source");
        ImGui::TableHeadersRow();

        DrawStatusRowText(
            snapshot,
            "Extended Slider Range",
            g_extendedSliderRange.load() ? "On" : "Off",
            false,
            "Global user preference. Allows eligible sliders to use wider expert ranges.");
        if (WeatherTickReady()) {
            DrawStatusRowBool(snapshot, "Force Clear Sky", snapshot.effective.forceClearSky, snapshot.regionSource.forceClearSky, "Crimson Weather currently forces clear skies.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "Force Clear Sky", RuntimeHookId::WeatherTick);
        }
        if (snapshot.effective.forceClearSky) {
            DrawStatusRowBlocked(snapshot, "Rain", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so rain is not applied.");
            DrawStatusRowBlocked(snapshot, "Thunder", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so thunder is not applied.");
            DrawStatusRowBlocked(snapshot, "Dust", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so dust is not applied.");
            DrawStatusRowBlocked(snapshot, "Snow", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so snow is not applied.");
        } else {
            if (!RainHookReady()) {
                DrawStatusRowHookDisabled(snapshot, "Rain", RuntimeHookId::GetRainIntensity);
            } else if (snapshot.effective.noRain) {
                DrawStatusRowBlocked(snapshot, "Rain", snapshot.regionSource.noRain, "No Rain is active, so Crimson Weather forces rain to zero.");
            } else {
                DrawStatusRowNativeFloat(snapshot, "Rain", snapshot.effective.rain, 0.0f, "%.3f", snapshot.regionSource.rain, "Crimson Weather currently forces rain to %s.");
            }
            if (WeatherTickReady()) {
                DrawStatusRowNativeFloat(snapshot, "Thunder", snapshot.effective.thunder, 0.0f, "%.3f", snapshot.regionSource.thunder, "Crimson Weather currently drives visual lightning and thunder SFX frequency at %s.");
            } else {
                DrawStatusRowHookDisabled(snapshot, "Thunder", RuntimeHookId::WeatherTick);
            }
            if (!DustHookReady()) {
                DrawStatusRowHookDisabled(snapshot, "Dust", RuntimeHookId::GetDustIntensity);
            } else if (snapshot.effective.noDust) {
                DrawStatusRowBlocked(snapshot, "Dust", snapshot.regionSource.noDust, "No Dust is active, so Crimson Weather forces dust to zero.");
            } else {
                DrawStatusRowNativeFloat(snapshot, "Dust", snapshot.effective.dust, 0.0f, "%.3f", snapshot.regionSource.dust, "Crimson Weather currently forces dust to %s.");
            }
            if (!SnowHookReady()) {
                DrawStatusRowHookDisabled(snapshot, "Snow", RuntimeHookId::GetSnowIntensity);
            } else if (snapshot.effective.noSnow) {
                DrawStatusRowBlocked(snapshot, "Snow", snapshot.regionSource.noSnow, "No Snow is active, so Crimson Weather forces snow to zero.");
            } else {
                DrawStatusRowNativeFloat(snapshot, "Snow", snapshot.effective.snow, 0.0f, "%.3f", snapshot.regionSource.snow, "Crimson Weather currently forces snow to %s.");
            }
        }
        if (RainHookReady()) {
            DrawStatusRowBool(snapshot, "No Rain", snapshot.effective.noRain, snapshot.regionSource.noRain, "Crimson Weather currently disables rain.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "No Rain", RuntimeHookId::GetRainIntensity);
        }
        if (DustHookReady()) {
            DrawStatusRowBool(snapshot, "No Dust", snapshot.effective.noDust, snapshot.regionSource.noDust, "Crimson Weather currently disables dust.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "No Dust", RuntimeHookId::GetDustIntensity);
        }
        if (SnowHookReady()) {
            DrawStatusRowBool(snapshot, "No Snow", snapshot.effective.noSnow, snapshot.regionSource.noSnow, "Crimson Weather currently disables snow.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "No Snow", RuntimeHookId::GetSnowIntensity);
        }

        if (WeatherTickReady()) {
            DrawStatusRowBool(snapshot, "Visual Time Override", snapshot.effective.visualTimeOverride, snapshot.regionSource.time, "Crimson Weather currently freezes visual time.");
            DrawStatusRowBool(snapshot, "Progress Visual Time", snapshot.effective.visualTimeOverride && snapshot.effective.progressVisualTime, snapshot.regionSource.time, "Crimson Weather currently advances the visual time override.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "Visual Time Override", RuntimeHookId::WeatherTick);
            DrawStatusRowHookDisabled(snapshot, "Progress Visual Time", RuntimeHookId::WeatherTick);
        }
        if (!WeatherTickReady()) {
            DrawStatusRowHookDisabled(snapshot, "Advance Interval", RuntimeHookId::WeatherTick);
        } else if (snapshot.effective.visualTimeOverride && snapshot.effective.progressVisualTime) {
            char intervalText[32] = {};
            FormatProgressVisualTimeInterval(snapshot.effective.progressVisualTimeIntervalMs, intervalText, sizeof(intervalText));
            char tooltipText[160] = {};
            sprintf_s(tooltipText, sizeof(tooltipText), "Crimson Weather currently advances the visual time override at %s.", intervalText);
            DrawStatusRowText(snapshot, "Advance Interval", intervalText, snapshot.regionSource.time, tooltipText);
        } else {
            DrawStatusRowText(snapshot, "Advance Interval", "DISABLED", snapshot.regionSource.time, "This only applies when Progress Visual Time is enabled.");
        }
        if (WeatherTickReady()) {
            DrawStatusRowActiveFloat(snapshot, "Time Hour", snapshot.effective.visualTimeOverride, NormalizeHour24(snapshot.effective.timeHour), "%.2f", snapshot.regionSource.time, "Crimson Weather currently forces time to %s.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "Time Hour", RuntimeHookId::WeatherTick);
        }

        if (!WindPackReady()) {
            DrawStatusRowHookDisabled(snapshot, "Cloud Amount", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Cloud Height", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Cloud Density", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Mid Clouds", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "High Clouds", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Cloud Alpha", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Cloud Phase Front", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Cloud Scattering Coefficient", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Rayleigh Scattering Color", RuntimeHookId::WindPack);
        } else if (snapshot.effective.forceClearSky) {
            DrawStatusRowBlocked(snapshot, "Cloud Amount", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud amount overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Height", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud height overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Density", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud density overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Mid Clouds", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so mid-cloud overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "High Clouds", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so high-cloud overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Alpha", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud alpha overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Phase Front", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud phase overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Scattering Coefficient", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud scattering overrides are not applied.");
        } else {
            DrawStatusRowEnabledFloat(snapshot, "Cloud Amount", snapshot.effective.cloudAmountEnabled, snapshot.effective.cloudAmount, "x%.2f", snapshot.regionSource.cloudAmount, "Crimson Weather currently multiplies cloud amount by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Height", snapshot.effective.cloudHeightEnabled, snapshot.effective.cloudHeight, "x%.2f", snapshot.regionSource.cloudHeight, "Crimson Weather currently multiplies cloud height by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Density", snapshot.effective.cloudDensityEnabled, snapshot.effective.cloudDensity, "x%.2f", snapshot.regionSource.cloudDensity, "Crimson Weather currently multiplies cloud density by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Mid Clouds", snapshot.effective.midCloudsEnabled, snapshot.effective.midClouds, "x%.2f", snapshot.regionSource.midClouds, "Crimson Weather currently multiplies mid clouds by %s.");
            DrawStatusRowEnabledFloat(snapshot, "High Clouds", snapshot.effective.highCloudsEnabled, snapshot.effective.highClouds, "x%.2f", snapshot.regionSource.highClouds, "Crimson Weather currently multiplies high clouds by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Alpha", snapshot.effective.cloudAlphaEnabled, snapshot.effective.cloudAlpha, "%.3f", snapshot.regionSource.cloudAlpha, "Crimson Weather currently sets cloud alpha to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Phase Front", snapshot.effective.cloudPhaseFrontEnabled, snapshot.effective.cloudPhaseFront, "%.4f", snapshot.regionSource.cloudPhaseFront, "Crimson Weather currently sets cloud phase front to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Scattering Coefficient", snapshot.effective.cloudScatteringCoefficientEnabled, snapshot.effective.cloudScatteringCoefficient, "%.5f", snapshot.regionSource.cloudScatteringCoefficient, "Crimson Weather currently sets cloud scattering coefficient to %s.");
        }
        if (WindPackReady()) {
            DrawStatusRowText(snapshot, "Rayleigh Scattering Color", snapshot.effective.rayleighScatteringColorEnabled ? "ACTIVE" : "NATIVE", snapshot.regionSource.rayleighScatteringColor, "Crimson Weather currently overrides Rayleigh scattering color.");
        }

        if (!WindPackReady()) {
            DrawStatusRowHookDisabled(snapshot, "2C", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "2D", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Cloud Variation", RuntimeHookId::WindPack);
        } else if (snapshot.effective.forceClearSky) {
            DrawStatusRowBlocked(snapshot, "2C", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so 2C cloud overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "2D", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so 2D cloud overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Variation", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud variation overrides are not applied.");
        } else {
            DrawStatusRowEnabledFloat(snapshot, "2C", snapshot.effective.exp2CEnabled, snapshot.effective.exp2C, "x%.2f", snapshot.regionSource.exp2C, "Crimson Weather currently multiplies 2C by %s.");
            DrawStatusRowEnabledFloat(snapshot, "2D", snapshot.effective.exp2DEnabled, snapshot.effective.exp2D, "x%.2f", snapshot.regionSource.exp2D, "Crimson Weather currently multiplies 2D by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Variation", snapshot.effective.cloudVariationEnabled, snapshot.effective.cloudVariation, "x%.2f", snapshot.regionSource.cloudVariation, "Crimson Weather currently multiplies cloud variation by %s.");
        }
        if (WeatherTickReady()) {
            DrawStatusRowEnabledFloat(snapshot, "Puddle Scale", snapshot.effective.puddleScaleEnabled, snapshot.effective.puddleScale, "%.3f", snapshot.regionSource.puddleScale, "Crimson Weather currently sets puddle scale to %s.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "Puddle Scale", RuntimeHookId::WeatherTick);
        }

        if (WindPackReady()) {
            DrawStatusRowEnabledFloat(snapshot, "Night Sky Tilt", snapshot.effective.nightSkyRotationEnabled, snapshot.effective.nightSkyRotation, "%.2f", snapshot.regionSource.nightSkyRotation, "Crimson Weather currently sets the native night sky tilt to %s.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "Night Sky Tilt", RuntimeHookId::WindPack);
        }
        if (SceneFrameReady()) {
            DrawStatusRowEnabledFloat(snapshot, "Night Sky Phase", snapshot.effective.nightSkyYawEnabled, snapshot.effective.nightSkyYaw, "%.2f", snapshot.regionSource.nightSkyYaw, "Crimson Weather currently sets the native night sky phase to %s.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "Night Sky Phase", RuntimeHookId::SceneFrameUpdate);
        }
        if (WindPackReady()) {
            DrawStatusRowEnabledFloat(snapshot, "Sun Light Intensity", snapshot.effective.sunLightIntensityEnabled, snapshot.effective.sunLightIntensity, "%.3f", snapshot.regionSource.sunLightIntensity, "Crimson Weather currently sets sun light intensity to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Sun Size", snapshot.effective.sunSizeEnabled, snapshot.effective.sunSize, "%.3f", snapshot.regionSource.sunSize, "Crimson Weather currently sets sun size to %s.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "Sun Light Intensity", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Sun Size", RuntimeHookId::WindPack);
        }
        if (SceneFrameReady()) {
            DrawStatusRowEnabledFloat(snapshot, "Sun Yaw Lock", snapshot.effective.sunYawEnabled, snapshot.effective.sunYaw, "%.2f", snapshot.regionSource.sunYaw, "Crimson Weather currently locks sun yaw to %s, overriding natural sun movement.");
            DrawStatusRowEnabledFloat(snapshot, "Sun Pitch Lock", snapshot.effective.sunPitchEnabled, snapshot.effective.sunPitch, "%.2f", snapshot.regionSource.sunPitch, "Crimson Weather currently locks sun pitch to %s, overriding natural sun movement.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "Sun Yaw Lock", RuntimeHookId::SceneFrameUpdate);
            DrawStatusRowHookDisabled(snapshot, "Sun Pitch Lock", RuntimeHookId::SceneFrameUpdate);
        }
        if (WindPackReady()) {
            DrawStatusRowEnabledFloat(snapshot, "Moon Light Intensity", snapshot.effective.moonLightIntensityEnabled, snapshot.effective.moonLightIntensity, "%.3f", snapshot.regionSource.moonLightIntensity, "Crimson Weather currently sets moon light intensity to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Moon Size", snapshot.effective.moonSizeEnabled, snapshot.effective.moonSize, "%.3f", snapshot.regionSource.moonSize, "Crimson Weather currently sets moon size to %s.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "Moon Light Intensity", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Moon Size", RuntimeHookId::WindPack);
        }
        if (SceneFrameReady()) {
            DrawStatusRowEnabledFloat(snapshot, "Moon Yaw Lock", snapshot.effective.moonYawEnabled, snapshot.effective.moonYaw, "%.2f", snapshot.regionSource.moonYaw, "Crimson Weather currently locks moon yaw to %s, overriding natural moon movement.");
            DrawStatusRowEnabledFloat(snapshot, "Moon Pitch Lock", snapshot.effective.moonPitchEnabled, snapshot.effective.moonPitch, "%.2f", snapshot.regionSource.moonPitch, "Crimson Weather currently locks moon pitch to %s, overriding natural moon movement.");
            DrawStatusRowEnabledFloat(snapshot, "Moon Rotation", snapshot.effective.moonRollEnabled, snapshot.effective.moonRoll, "%.2f", snapshot.regionSource.moonRoll, "Crimson Weather currently rotates the moon disc to %s.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "Moon Yaw Lock", RuntimeHookId::SceneFrameUpdate);
            DrawStatusRowHookDisabled(snapshot, "Moon Pitch Lock", RuntimeHookId::SceneFrameUpdate);
            DrawStatusRowHookDisabled(snapshot, "Moon Rotation", RuntimeHookId::SceneFrameUpdate);
        }
        DrawStatusRowText(
            snapshot,
            "Moon Texture",
            (snapshot.effective.moonTextureEnabled && !snapshot.effective.moonTexture.empty()) ? snapshot.effective.moonTexture.c_str() : "NATIVE",
            snapshot.regionSource.moonTexture,
            "Crimson Weather currently swaps the moon texture.");
        DrawStatusRowText(
            snapshot,
            "Milky Way Texture",
            (snapshot.effective.milkywayTextureEnabled && !snapshot.effective.milkywayTexture.empty()) ? snapshot.effective.milkywayTexture.c_str() : "NATIVE",
            snapshot.regionSource.milkywayTexture,
            "Crimson Weather currently swaps the Milky Way texture.");

        const bool fogForcedZero = snapshot.effective.forceClearSky || snapshot.effective.noFog;
        const bool fogForceSource = snapshot.effective.forceClearSky ? snapshot.regionSource.forceClearSky : snapshot.regionSource.noFog;
        const char* fogForceTooltip = snapshot.effective.forceClearSky
            ? "Force Clear Sky is active, so Crimson Weather forces legacy fog to zero."
            : "No Fog is active, so Crimson Weather forces legacy fog to zero.";
        const char* nativeFogForceTooltip = snapshot.effective.forceClearSky
            ? "Force Clear Sky is active, so Crimson Weather forces native fog to zero."
            : "No Fog is active, so Crimson Weather forces native fog to zero.";
        if (!WeatherFrameReady()) {
            DrawStatusRowHookDisabled(snapshot, "Fog [LEGACY]", RuntimeHookId::WeatherFrameUpdate);
        } else if (fogForcedZero) {
            DrawStatusRowBlocked(snapshot, "Fog [LEGACY]", fogForceSource, fogForceTooltip);
        } else {
            DrawStatusRowEnabledFloat(snapshot, "Fog [LEGACY]", snapshot.effective.fogEnabled, snapshot.effective.fogPercent, "%.1f%%", snapshot.regionSource.fog, "Crimson Weather currently forces legacy fog to %s.");
        }
        if (!WindPackReady()) {
            DrawStatusRowHookDisabled(snapshot, "Fog", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Volume Fog Scatter Color", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Aerosol Height", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Aerosol Density", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Aerosol Absorption", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Fog Height Baseline", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Fog Height Falloff", RuntimeHookId::WindPack);
        } else if (fogForcedZero) {
            DrawStatusRowBlocked(snapshot, "Fog", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Volume Fog Scatter Color", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Aerosol Height", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Aerosol Density", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Aerosol Absorption", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Fog Height Baseline", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Fog Height Falloff", fogForceSource, nativeFogForceTooltip);
        } else {
            DrawStatusRowEnabledFloat(snapshot, "Fog", snapshot.effective.nativeFogEnabled, snapshot.effective.nativeFog, "%.2f", snapshot.regionSource.nativeFog, "Crimson Weather currently scales native fog by %s.");
            DrawStatusRowText(snapshot, "Volume Fog Scatter Color", snapshot.effective.volumeFogScatterColorEnabled ? "ACTIVE" : "NATIVE", snapshot.regionSource.volumeFogScatterColor, "Crimson Weather currently overrides volume fog scatter color.");
            DrawStatusRowEnabledFloat(snapshot, "Aerosol Height", snapshot.effective.mieScaleHeightEnabled, snapshot.effective.mieScaleHeight, "%.1f", snapshot.regionSource.mieScaleHeight, "Crimson Weather currently sets aerosol height to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Aerosol Density", snapshot.effective.mieAerosolDensityEnabled, snapshot.effective.mieAerosolDensity, "%.4f", snapshot.regionSource.mieAerosolDensity, "Crimson Weather currently sets aerosol density to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Aerosol Absorption", snapshot.effective.mieAerosolAbsorptionEnabled, snapshot.effective.mieAerosolAbsorption, "%.4f", snapshot.regionSource.mieAerosolAbsorption, "Crimson Weather currently sets aerosol absorption to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Fog Height Baseline", snapshot.effective.heightFogBaselineEnabled, snapshot.effective.heightFogBaseline, "%.1f", snapshot.regionSource.heightFogBaseline, "Crimson Weather currently sets fog height baseline to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Fog Height Falloff", snapshot.effective.heightFogFalloffEnabled, snapshot.effective.heightFogFalloff, "%.4f", snapshot.regionSource.heightFogFalloff, "Crimson Weather currently sets fog height falloff to %s.");
        }
        if (WeatherFrameReady() || WindPackReady()) {
            DrawStatusRowBool(snapshot, "No Fog", snapshot.effective.noFog, snapshot.regionSource.noFog, "Crimson Weather currently disables fog.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "No Fog", RuntimeHookId::WeatherFrameUpdate);
        }
        if (!WindPackReady()) {
            DrawStatusRowHookDisabled(snapshot, "Wind", RuntimeHookId::WindPack);
        } else if (snapshot.effective.noWind) {
            DrawStatusRowBlocked(snapshot, "Wind", snapshot.regionSource.noWind, "No Wind is active, so the wind multiplier is not applied.");
        } else {
            DrawStatusRowNativeFloat(snapshot, "Wind", snapshot.effective.wind, 1.0f, "x%.2f", snapshot.regionSource.wind, "Crimson Weather currently multiplies wind by %s.");
        }
        if (WeatherTickReady()) {
            DrawStatusRowBool(snapshot, "No Wind", snapshot.effective.noWind, snapshot.regionSource.noWind, "Crimson Weather currently disables wind.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "No Wind", RuntimeHookId::WeatherTick);
        }

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
    ImGui::InputTextWithHint("##preset_filter", "Search...", g_presetFilter, IM_ARRAYSIZE(g_presetFilter));
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
    const bool forceClearEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::ForceClear) && WeatherTickReady();
    if (!forceClearEnabled) {
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
    if (!forceClearEnabled) {
        ImGui::EndDisabled();
        if (!RuntimeFeatureAvailable(RuntimeFeatureId::ForceClear)) {
            DrawFeatureUnavailable(RuntimeFeatureId::ForceClear);
        } else {
            DrawHookUnavailable(RuntimeHookId::WeatherTick);
        }
    }

    bool noRain = detachedEdit ? editData.noRain : g_noRain.load();
    bool noDust = detachedEdit ? editData.noDust : g_noDust.load();
    bool noSnow = detachedEdit ? editData.noSnow : g_noSnow.load();

    float rain = detachedEdit ? editData.rain : (g_oRain.active.load() ? g_oRain.value.load() : 0.0f);
    const SliderRange rainRange = ActiveSliderRange(0.0f, 1.0f, 0.0f, 5.0f);
    const bool rainNative = rain <= 0.0001f;
    const bool rainEnabled = !forceClear && !noRain && RuntimeFeatureAvailable(RuntimeFeatureId::Rain) && RainHookReady();
    if (!rainEnabled) {
        ImGui::BeginDisabled();
    }
    bool rainChanged = false;
    bool rainOverrideChanged = false;
    if (DrawSliderFloatRow("Rain", "rain", &rain, rainRange.lo, rainRange.hi, "%.3f", &rainChanged, regionScoped ? &overrideMask.rain : nullptr, &rainOverrideChanged, rainNative)) {
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
            editData.rain = ClampSliderValue(rain, rainRange);
            if (regionScoped) overrideMask.rain = true;
            editChanged = true;
        } else {
            rain = ClampSliderValue(rain, rainRange);
            rain > 0.0001f ? g_oRain.set(rain) : g_oRain.clear();
        }
    }
    if (!rainEnabled) {
        ImGui::EndDisabled();
        if (!forceClear && !noRain) {
            if (!RuntimeFeatureAvailable(RuntimeFeatureId::Rain)) {
                DrawFeatureUnavailable(RuntimeFeatureId::Rain);
            } else {
                DrawHookUnavailable(RuntimeHookId::GetRainIntensity);
            }
        }
    }

    float thunder = detachedEdit ? editData.thunder : (g_oThunder.active.load() ? g_oThunder.value.load() : 0.0f);
    const SliderRange thunderRange = ActiveSliderRange(0.0f, 1.0f, 0.0f, 5.0f);
    const bool thunderNative = thunder <= 0.0001f;
    const bool thunderEnabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::ThunderControls) && WeatherTickReady();
    if (!thunderEnabled) {
        ImGui::BeginDisabled();
    }
    bool thunderChanged = false;
    bool thunderOverrideChanged = false;
    if (DrawSliderFloatRow("Thunder", "thunder", &thunder, thunderRange.lo, thunderRange.hi, "%.3f", &thunderChanged, regionScoped ? &overrideMask.thunder : nullptr, &thunderOverrideChanged, thunderNative)) {
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
            editData.thunder = ClampSliderValue(thunder, thunderRange);
            if (regionScoped) overrideMask.thunder = true;
            editChanged = true;
        } else {
            thunder = ClampSliderValue(thunder, thunderRange);
            thunder > 0.0001f ? g_oThunder.set(thunder) : g_oThunder.clear();
        }
    }
    if (!thunderEnabled) {
        ImGui::EndDisabled();
        if (!RuntimeFeatureAvailable(RuntimeFeatureId::ThunderControls)) {
            DrawFeatureUnavailable(RuntimeFeatureId::ThunderControls);
        } else if (!forceClear) {
            DrawHookUnavailable(RuntimeHookId::WeatherTick);
        }
    }

    float dust = detachedEdit ? editData.dust : (g_oDust.active.load() ? g_oDust.value.load() : 0.0f);
    const SliderRange dustRange = ActiveSliderRange(0.0f, 2.0f, 0.0f, 10.0f);
    const bool dustNative = dust <= 0.0001f;
    const bool dustEnabled = !forceClear && !noDust && RuntimeFeatureAvailable(RuntimeFeatureId::Dust) && DustHookReady();
    if (!dustEnabled) {
        ImGui::BeginDisabled();
    }
    bool dustChanged = false;
    bool dustOverrideChanged = false;
    if (DrawSliderFloatRow("Dust", "dust", &dust, dustRange.lo, dustRange.hi, "%.3f", &dustChanged, regionScoped ? &overrideMask.dust : nullptr, &dustOverrideChanged, dustNative)) {
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
            editData.dust = ClampSliderValue(dust, dustRange);
            if (regionScoped) overrideMask.dust = true;
            editChanged = true;
        } else {
            dust = ClampSliderValue(dust, dustRange);
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
            if (!RuntimeFeatureAvailable(RuntimeFeatureId::Dust)) {
                DrawFeatureUnavailable(RuntimeFeatureId::Dust);
            } else {
                DrawHookUnavailable(RuntimeHookId::GetDustIntensity);
            }
        }
    }

    float snow = detachedEdit ? editData.snow : (g_oSnow.active.load() ? g_oSnow.value.load() : 0.0f);
    const SliderRange snowRange = ActiveSliderRange(0.0f, 1.0f, 0.0f, 5.0f);
    const bool snowNative = snow <= 0.0001f;
    const bool snowEnabled = !forceClear && !noSnow && RuntimeFeatureAvailable(RuntimeFeatureId::Snow) && SnowHookReady();
    if (!snowEnabled) {
        ImGui::BeginDisabled();
    }
    bool snowChanged = false;
    bool snowOverrideChanged = false;
    if (DrawSliderFloatRow("Snow", "snow", &snow, snowRange.lo, snowRange.hi, "%.3f", &snowChanged, regionScoped ? &overrideMask.snow : nullptr, &snowOverrideChanged, snowNative)) {
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
            editData.snow = ClampSliderValue(snow, snowRange);
            if (regionScoped) overrideMask.snow = true;
            editChanged = true;
        } else {
            snow = ClampSliderValue(snow, snowRange);
            snow > 0.0001f ? g_oSnow.set(snow) : g_oSnow.clear();
        }
    }
    if (!snowEnabled) {
        ImGui::EndDisabled();
        if (!forceClear && !noSnow) {
            if (!RuntimeFeatureAvailable(RuntimeFeatureId::Snow)) {
                DrawFeatureUnavailable(RuntimeFeatureId::Snow);
            } else {
                DrawHookUnavailable(RuntimeHookId::GetSnowIntensity);
            }
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Disable Native Weather");
    const bool noRainEnabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Rain) && RainHookReady();
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
        if (!forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Rain)) {
            DrawHookUnavailable(RuntimeHookId::GetRainIntensity);
        }
    }

    const bool noDustEnabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Dust) && DustHookReady();
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
        if (!forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Dust)) {
            DrawHookUnavailable(RuntimeHookId::GetDustIntensity);
        }
    }

    const bool noSnowEnabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Snow) && SnowHookReady();
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
        if (!forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::Snow)) {
            DrawHookUnavailable(RuntimeHookId::GetSnowIntensity);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Time");
    bool visualTimeOverride = detachedEdit ? editData.visualTimeOverride : (g_timeCtrlActive.load() && g_timeFreeze.load());
    bool progressVisualTime = detachedEdit ? editData.progressVisualTime : g_timeProgressVisualTime.load();
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
    if (timeOverrideChanged) {
        editChanged = true;
    }
    if (visualTimeChanged) {
        if (detachedEdit) {
            editData.visualTimeOverride = visualTimeOverride;
            if (!visualTimeOverride) editData.progressVisualTime = false;
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeCtrlActive.store(visualTimeOverride);
            g_timeFreeze.store(visualTimeOverride);
            if (!visualTimeOverride) {
                g_timeProgressVisualTime.store(false);
                g_timeProgressLastTick.store(0);
            }
            g_timeApplyRequest.store(true);
        }
        GUI_SetStatus(visualTimeOverride ? "Visual time override enabled" : "Visual time override disabled");
    }
    if (!visualTimeOverride) {
        progressVisualTime = false;
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
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeProgressVisualTime.store(progressVisualTime);
            g_timeProgressLastTick.store(progressVisualTime ? GetTickCount64() : 0);
            g_timeCtrlActive.store(visualTimeOverride);
            g_timeFreeze.store(visualTimeOverride);
            g_timeApplyRequest.store(true);
        }
        GUI_SetStatus(progressVisualTime ? "Progress Visual Time enabled" : "Progress Visual Time disabled");
    }
    const bool intervalDisabled = progressDisabled || !progressVisualTime;
    if (intervalDisabled) {
        ImGui::BeginDisabled();
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Advance Interval");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("Controls how often Progress Visual Time steps forward. 0 ms updates every frame.");
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(125.0f);
    const bool progressIntervalChanged = ImGui::SliderFloat("##progress_visual_time_interval", &progressVisualTimeIntervalMs, 0.0f, 5000.0f, "%.0f ms");
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
            g_timeProgressLastTick.store(0);
        }
        GUI_SetStatus("Advance interval changed");
    }
    if (ClampProgressVisualTimeIntervalMs(progressVisualTimeIntervalMs) <= 0.5f) {
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
    const float displayedTimeHour = runtimeProgressVisualTimeActive
        ? g_timeTargetHour.load()
        : (detachedEdit ? editData.timeHour : g_timeTargetHour.load());
    int timeMinutes = progressUiActive
        ? HourToMinuteOfDayFloor(displayedTimeHour)
        : HourToMinuteOfDay(displayedTimeHour);
    char targetClock[32] = {};
    FormatGameClockFromMinute(timeMinutes, targetClock, sizeof(targetClock));

    if (!(timeEnabled && visualTimeOverride) || timeValueLocked) {
        ImGui::BeginDisabled();
    }
    const bool timeChanged = DrawClockDial("time", &timeMinutes);
    const bool dialActive = ImGui::IsItemActive();

    if (timeChanged && timeEnabled && visualTimeOverride) {
        if (detachedEdit) {
            editData.timeHour = MinuteOfDayToHour(timeMinutes);
            editData.visualTimeOverride = true;
            editData.progressVisualTime = progressVisualTime;
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeTargetHour.store(MinuteOfDayToHour(timeMinutes));
            g_timeCtrlActive.store(true);
            g_timeFreeze.store(true);
            g_timeProgressLastTick.store(g_timeProgressVisualTime.load() ? GetTickCount64() : 0);
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
            editData.progressVisualTime = progressVisualTime;
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeTargetHour.store(MinuteOfDayToHour(HourToMinuteOfDay(g_timeCurrentHour.load())));
            g_timeCtrlActive.store(true);
            g_timeFreeze.store(true);
            g_timeProgressLastTick.store(g_timeProgressVisualTime.load() ? GetTickCount64() : 0);
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
            editData.progressVisualTime = progressVisualTime;
            if (regionScoped) overrideMask.time = true;
            editChanged = true;
        } else {
            g_timeTargetHour.store(MinuteOfDayToHour(timeMinutes));
            g_timeCtrlActive.store(true);
            g_timeFreeze.store(true);
            g_timeProgressLastTick.store(g_timeProgressVisualTime.load() ? GetTickCount64() : 0);
            g_timeApplyRequest.store(true);
        }
    }
    if (!(timeEnabled && visualTimeOverride) || timeValueLocked) {
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Wind");
    bool noWind = detachedEdit ? editData.noWind : g_noWind.load();
    const bool windEnabled = !noWind && RuntimeFeatureAvailable(RuntimeFeatureId::WindControls) && WindPackReady();
    float wind = detachedEdit ? editData.wind : g_windMul.load();
    const SliderRange windRange = ActiveSliderRange(0.0f, 15.0f, 0.0f, 50.0f);
    const bool windNative = fabsf(wind - 1.0f) <= 0.001f;
    if (!windEnabled) {
        ImGui::BeginDisabled();
    }
    bool windChanged = false;
    bool windOverrideChanged = false;
    if (DrawSliderFloatRow("Wind", "wind_general", &wind, windRange.lo, windRange.hi, "x%.2f", &windChanged, regionScoped ? &overrideMask.wind : nullptr, &windOverrideChanged, windNative)) {
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
            editData.wind = ClampSliderValue(wind, windRange);
            if (regionScoped) overrideMask.wind = true;
            editChanged = true;
        } else {
            g_windMul.store(ClampSliderValue(wind, windRange));
        }
    }
    if (!windEnabled) {
        ImGui::EndDisabled();
        if (!noWind) {
            if (!RuntimeFeatureAvailable(RuntimeFeatureId::WindControls)) {
                DrawFeatureUnavailable(RuntimeFeatureId::WindControls);
            } else {
                DrawHookUnavailable(RuntimeHookId::WindPack);
            }
        }
    }

    const bool noWindEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::NoWindControls) && WeatherTickReady();
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
        if (!RuntimeFeatureAvailable(RuntimeFeatureId::NoWindControls)) {
            DrawFeatureUnavailable(RuntimeFeatureId::NoWindControls);
        } else {
            DrawHookUnavailable(RuntimeHookId::WeatherTick);
        }
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

    const bool windPackAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::CloudControls) && WindPackReady();
    ImGui::SeparatorText("Rayleigh");
    if (!windPackAvailable) {
        ImGui::BeginDisabled();
    }
    const WeatherPresetColor nativeRayleigh = RayleighColorFromUiBits(g_windPackBase0FBits.load());
    WeatherPresetColor rayleighColorData = detachedEdit
        ? (editData.rayleighScatteringColorEnabled ? editData.rayleighScatteringColor : nativeRayleigh)
        : (g_oRayleighScatteringColor.active.load()
            ? WeatherPresetColor{ g_oRayleighScatteringColor.r.load(), g_oRayleighScatteringColor.g.load(), g_oRayleighScatteringColor.b.load(), 1.0f }
            : nativeRayleigh);
    float rayleighColor[4] = { rayleighColorData.r, rayleighColorData.g, rayleighColorData.b, 1.0f };
    const bool rayleighNative = detachedEdit ? !editData.rayleighScatteringColorEnabled : !g_oRayleighScatteringColor.active.load();
    bool rayleighColorChanged = false;
    bool rayleighColorOverrideChanged = false;
    if (DrawColorRow("Rayleigh Scattering Color", "rayleigh_scattering_color", rayleighColor, false, &rayleighColorChanged, regionScoped ? &overrideMask.rayleighScatteringColor : nullptr, &rayleighColorOverrideChanged, rayleighNative)) {
        if (detachedEdit) {
            editData.rayleighScatteringColorEnabled = false;
            editData.rayleighScatteringColor = nativeRayleigh;
            if (regionScoped) overrideMask.rayleighScatteringColor = true;
            editChanged = true;
        } else {
            g_oRayleighScatteringColor.clear();
        }
    } else if (rayleighColorOverrideChanged) {
        editChanged = true;
    } else if (windPackAvailable && rayleighColorChanged) {
        ClampColorValues(rayleighColor, false);
        if (detachedEdit) {
            editData.rayleighScatteringColor = ColorFromArray(rayleighColor, false);
            editData.rayleighScatteringColorEnabled = true;
            if (regionScoped) overrideMask.rayleighScatteringColor = true;
            editChanged = true;
        } else {
            g_oRayleighScatteringColor.set(rayleighColor[0], rayleighColor[1], rayleighColor[2], 1.0f);
        }
    }
    if (!windPackAvailable) {
        ImGui::EndDisabled();
        DrawHookUnavailable(RuntimeHookId::WindPack);
    }

    ImGui::Spacing();
    const bool cloudEnabled = !(detachedEdit ? editData.forceClearSky : g_forceClear.load()) &&
                              RuntimeFeatureAvailable(RuntimeFeatureId::CloudControls) &&
                              WindPackReady();
    ImGui::SeparatorText("Clouds");
    if (!cloudEnabled) {
        ImGui::BeginDisabled();
    }

    float cloudAmount = detachedEdit ? editData.cloudAmount : (g_oCloudAmount.active.load() ? g_oCloudAmount.value.load() : 1.0f);
    const bool cloudAmountNative = detachedEdit ? !editData.cloudAmountEnabled : !g_oCloudAmount.active.load();
    bool cloudAmountChanged = false;
    bool cloudAmountOverrideChanged = false;
    const SliderRange cloudAmountRange = ActiveSliderRange(0.0f, 15.0f, 0.0f, 50.0f);
    if (DrawSliderFloatRow("Cloud Amount", "cloud_amount", &cloudAmount, cloudAmountRange.lo, cloudAmountRange.hi, "x%.2f", &cloudAmountChanged, regionScoped ? &overrideMask.cloudAmount : nullptr, &cloudAmountOverrideChanged, cloudAmountNative)) {
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
            editData.cloudAmount = ClampSliderValue(cloudAmount, cloudAmountRange);
            editData.cloudAmountEnabled = fabsf(editData.cloudAmount - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.cloudAmount = true;
            editChanged = true;
        } else {
            cloudAmount = ClampSliderValue(cloudAmount, cloudAmountRange);
            fabsf(cloudAmount - 1.0f) <= 0.001f ? g_oCloudAmount.clear() : g_oCloudAmount.set(cloudAmount);
        }
    }

    float cloudHeight = detachedEdit ? editData.cloudHeight : (g_oCloudSpdX.active.load() ? g_oCloudSpdX.value.load() : 1.0f);
    const bool cloudHeightNative = detachedEdit ? !editData.cloudHeightEnabled : !g_oCloudSpdX.active.load();
    bool cloudHeightChanged = false;
    bool cloudHeightOverrideChanged = false;
    const SliderRange cloudHeightRange = ActiveSliderRange(-15.0f, 15.0f, -50.0f, 50.0f);
    if (DrawSliderFloatRow("Cloud Height", "cloud_height", &cloudHeight, cloudHeightRange.lo, cloudHeightRange.hi, "x%.2f", &cloudHeightChanged, regionScoped ? &overrideMask.cloudHeight : nullptr, &cloudHeightOverrideChanged, cloudHeightNative)) {
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
            editData.cloudHeight = ClampSliderValue(cloudHeight, cloudHeightRange);
            editData.cloudHeightEnabled = fabsf(editData.cloudHeight - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.cloudHeight = true;
            editChanged = true;
        } else {
            cloudHeight = ClampSliderValue(cloudHeight, cloudHeightRange);
            fabsf(cloudHeight - 1.0f) <= 0.001f ? g_oCloudSpdX.clear() : g_oCloudSpdX.set(cloudHeight);
        }
    }

    float cloudDensity = detachedEdit ? editData.cloudDensity : (g_oCloudSpdY.active.load() ? g_oCloudSpdY.value.load() : 1.0f);
    const bool cloudDensityNative = detachedEdit ? !editData.cloudDensityEnabled : !g_oCloudSpdY.active.load();
    bool cloudDensityChanged = false;
    bool cloudDensityOverrideChanged = false;
    const SliderRange cloudDensityRange = ActiveSliderRange(0.0f, 10.0f, 0.0f, 50.0f);
    if (DrawSliderFloatRow("Cloud Density", "cloud_density", &cloudDensity, cloudDensityRange.lo, cloudDensityRange.hi, "x%.2f", &cloudDensityChanged, regionScoped ? &overrideMask.cloudDensity : nullptr, &cloudDensityOverrideChanged, cloudDensityNative)) {
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
            editData.cloudDensity = ClampSliderValue(cloudDensity, cloudDensityRange);
            editData.cloudDensityEnabled = fabsf(editData.cloudDensity - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.cloudDensity = true;
            editChanged = true;
        } else {
            cloudDensity = ClampSliderValue(cloudDensity, cloudDensityRange);
            fabsf(cloudDensity - 1.0f) <= 0.001f ? g_oCloudSpdY.clear() : g_oCloudSpdY.set(cloudDensity);
        }
    }

    float midClouds = detachedEdit ? editData.midClouds : (g_oHighClouds.active.load() ? g_oHighClouds.value.load() : 1.0f);
    const bool midCloudsNative = detachedEdit ? !editData.midCloudsEnabled : !g_oHighClouds.active.load();
    bool midCloudsChanged = false;
    bool midCloudsOverrideChanged = false;
    const SliderRange midCloudsRange = ActiveSliderRange(0.0f, 15.0f, 0.0f, 50.0f);
    if (DrawSliderFloatRow("Mid Clouds", "mid_clouds", &midClouds, midCloudsRange.lo, midCloudsRange.hi, "x%.2f", &midCloudsChanged, regionScoped ? &overrideMask.midClouds : nullptr, &midCloudsOverrideChanged, midCloudsNative)) {
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
            editData.midClouds = ClampSliderValue(midClouds, midCloudsRange);
            editData.midCloudsEnabled = fabsf(editData.midClouds - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.midClouds = true;
            editChanged = true;
        } else {
            midClouds = ClampSliderValue(midClouds, midCloudsRange);
            fabsf(midClouds - 1.0f) <= 0.001f ? g_oHighClouds.clear() : g_oHighClouds.set(midClouds);
        }
    }

    float highClouds = detachedEdit ? editData.highClouds : (g_oAtmoAlpha.active.load() ? g_oAtmoAlpha.value.load() : 1.0f);
    const bool highCloudsNative = detachedEdit ? !editData.highCloudsEnabled : !g_oAtmoAlpha.active.load();
    bool highCloudsChanged = false;
    bool highCloudsOverrideChanged = false;
    const SliderRange highCloudsRange = ActiveSliderRange(0.0f, 15.0f, 0.0f, 50.0f);
    if (DrawSliderFloatRow("High Clouds", "high_clouds", &highClouds, highCloudsRange.lo, highCloudsRange.hi, "x%.2f", &highCloudsChanged, regionScoped ? &overrideMask.highClouds : nullptr, &highCloudsOverrideChanged, highCloudsNative)) {
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
            editData.highClouds = ClampSliderValue(highClouds, highCloudsRange);
            editData.highCloudsEnabled = fabsf(editData.highClouds - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.highClouds = true;
            editChanged = true;
        } else {
            highClouds = ClampSliderValue(highClouds, highCloudsRange);
            fabsf(highClouds - 1.0f) <= 0.001f ? g_oAtmoAlpha.clear() : g_oAtmoAlpha.set(highClouds);
        }
    }

    float cloudAlpha = detachedEdit ? editData.cloudAlpha : (g_oCloudAlpha.active.load() ? g_oCloudAlpha.value.load() : g_windPackBase1E.load());
    const bool cloudAlphaNative = detachedEdit ? !editData.cloudAlphaEnabled : !g_oCloudAlpha.active.load();
    bool cloudAlphaChanged = false;
    bool cloudAlphaOverrideChanged = false;
    const SliderRange cloudAlphaRange = ActiveSliderRange(0.0f, 50.0f, 0.0f, 100.0f);
    if (DrawSliderFloatRow("Cloud Alpha", "cloud_alpha", &cloudAlpha, cloudAlphaRange.lo, cloudAlphaRange.hi, "%.3f", &cloudAlphaChanged, regionScoped ? &overrideMask.cloudAlpha : nullptr, &cloudAlphaOverrideChanged, cloudAlphaNative)) {
        if (detachedEdit) {
            editData.cloudAlphaEnabled = false;
            editData.cloudAlpha = g_windPackBase1E.load();
            if (regionScoped) overrideMask.cloudAlpha = true;
            editChanged = true;
        } else {
            g_oCloudAlpha.clear();
        }
    } else if (cloudAlphaOverrideChanged) {
        editChanged = true;
    } else if (cloudEnabled && cloudAlphaChanged) {
        if (detachedEdit) {
            editData.cloudAlpha = ClampSliderValue(cloudAlpha, cloudAlphaRange);
            editData.cloudAlphaEnabled = true;
            if (regionScoped) overrideMask.cloudAlpha = true;
            editChanged = true;
        } else {
            g_oCloudAlpha.set(ClampSliderValue(cloudAlpha, cloudAlphaRange));
        }
    }

    float cloudPhaseFront = detachedEdit ? editData.cloudPhaseFront : (g_oCloudPhaseFront.active.load() ? g_oCloudPhaseFront.value.load() : g_windPackBase21.load());
    const bool cloudPhaseFrontNative = detachedEdit ? !editData.cloudPhaseFrontEnabled : !g_oCloudPhaseFront.active.load();
    bool cloudPhaseFrontChanged = false;
    bool cloudPhaseFrontOverrideChanged = false;
    const SliderRange cloudPhaseFrontRange = ActiveSliderRange(-1.0f, 1.0f, -1.0f, 1.0f);
    if (DrawSliderFloatRow("Cloud Phase Front", "cloud_phase_front", &cloudPhaseFront, cloudPhaseFrontRange.lo, cloudPhaseFrontRange.hi, "%.4f", &cloudPhaseFrontChanged, regionScoped ? &overrideMask.cloudPhaseFront : nullptr, &cloudPhaseFrontOverrideChanged, cloudPhaseFrontNative)) {
        if (detachedEdit) {
            editData.cloudPhaseFrontEnabled = false;
            editData.cloudPhaseFront = g_windPackBase21.load();
            if (regionScoped) overrideMask.cloudPhaseFront = true;
            editChanged = true;
        } else {
            g_oCloudPhaseFront.clear();
        }
    } else if (cloudPhaseFrontOverrideChanged) {
        editChanged = true;
    } else if (cloudEnabled && cloudPhaseFrontChanged) {
        if (detachedEdit) {
            editData.cloudPhaseFront = ClampSliderValue(cloudPhaseFront, cloudPhaseFrontRange);
            editData.cloudPhaseFrontEnabled = true;
            if (regionScoped) overrideMask.cloudPhaseFront = true;
            editChanged = true;
        } else {
            g_oCloudPhaseFront.set(ClampSliderValue(cloudPhaseFront, cloudPhaseFrontRange));
        }
    }

    float cloudScatter = detachedEdit ? editData.cloudScatteringCoefficient : (g_oCloudScatteringCoefficient.active.load() ? g_oCloudScatteringCoefficient.value.load() : g_windPackBase20.load());
    const bool cloudScatterNative = detachedEdit ? !editData.cloudScatteringCoefficientEnabled : !g_oCloudScatteringCoefficient.active.load();
    bool cloudScatterChanged = false;
    bool cloudScatterOverrideChanged = false;
    const SliderRange cloudScatterRange = ActiveSliderRange(kCloudScatteringCoefficientMin, 1.0f, kCloudScatteringCoefficientMin, 100.0f);
    if (DrawSliderFloatRow("Cloud Scattering Coefficient", "cloud_scattering_coefficient", &cloudScatter, cloudScatterRange.lo, cloudScatterRange.hi, "%.5f", &cloudScatterChanged, regionScoped ? &overrideMask.cloudScatteringCoefficient : nullptr, &cloudScatterOverrideChanged, cloudScatterNative)) {
        if (detachedEdit) {
            editData.cloudScatteringCoefficientEnabled = false;
            editData.cloudScatteringCoefficient = g_windPackBase20.load();
            if (regionScoped) overrideMask.cloudScatteringCoefficient = true;
            editChanged = true;
        } else {
            g_oCloudScatteringCoefficient.clear();
        }
    } else if (cloudScatterOverrideChanged) {
        editChanged = true;
    } else if (cloudEnabled && cloudScatterChanged) {
        if (detachedEdit) {
            editData.cloudScatteringCoefficient = ClampSliderValue(cloudScatter, cloudScatterRange);
            editData.cloudScatteringCoefficientEnabled = true;
            if (regionScoped) overrideMask.cloudScatteringCoefficient = true;
            editChanged = true;
        } else {
            g_oCloudScatteringCoefficient.set(ClampSliderValue(cloudScatter, cloudScatterRange));
        }
    }

    if (!cloudEnabled) {
        ImGui::EndDisabled();
        if (!RuntimeFeatureAvailable(RuntimeFeatureId::CloudControls)) {
            DrawFeatureUnavailable(RuntimeFeatureId::CloudControls);
        } else if (!(detachedEdit ? editData.forceClearSky : g_forceClear.load())) {
            DrawHookUnavailable(RuntimeHookId::WindPack);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Fog");

    float fogFromWind = detachedEdit ? editData.nativeFog : (g_oNativeFog.active.load() ? g_oNativeFog.value.load() : 1.0f);
    const bool fogFromWindNative = detachedEdit ? !editData.nativeFogEnabled : !g_oNativeFog.active.load();
    const bool fogBlocked = detachedEdit ? (editData.forceClearSky || editData.noFog) : (g_forceClear.load() || g_noFog.load());
    const bool windEnabled = !fogBlocked && RuntimeFeatureAvailable(RuntimeFeatureId::WindControls) && WindPackReady();
    if (!windEnabled) {
        ImGui::BeginDisabled();
    }

    WeatherPresetColor nativeVolumeFogColor{
        g_windPackBase34.load(),
        g_windPackBase35.load(),
        g_windPackBase36.load(),
        g_windPackBase37.load(),
    };
    WeatherPresetColor volumeFogColorData = detachedEdit
        ? (editData.volumeFogScatterColorEnabled ? editData.volumeFogScatterColor : nativeVolumeFogColor)
        : (g_oVolumeFogScatterColor.active.load()
            ? WeatherPresetColor{ g_oVolumeFogScatterColor.r.load(), g_oVolumeFogScatterColor.g.load(), g_oVolumeFogScatterColor.b.load(), g_oVolumeFogScatterColor.a.load() }
            : nativeVolumeFogColor);
    float volumeFogColor[4] = { volumeFogColorData.r, volumeFogColorData.g, volumeFogColorData.b, volumeFogColorData.a };
    const bool volumeFogNative = detachedEdit ? !editData.volumeFogScatterColorEnabled : !g_oVolumeFogScatterColor.active.load();
    bool volumeFogColorChanged = false;
    bool volumeFogColorOverrideChanged = false;
    if (DrawColorRow("Volume Fog Scatter Color", "volume_fog_scatter_color", volumeFogColor, true, &volumeFogColorChanged, regionScoped ? &overrideMask.volumeFogScatterColor : nullptr, &volumeFogColorOverrideChanged, volumeFogNative)) {
        if (detachedEdit) {
            editData.volumeFogScatterColorEnabled = false;
            editData.volumeFogScatterColor = nativeVolumeFogColor;
            if (regionScoped) overrideMask.volumeFogScatterColor = true;
            editChanged = true;
        } else {
            g_oVolumeFogScatterColor.clear();
        }
    } else if (volumeFogColorOverrideChanged) {
        editChanged = true;
    } else if (windEnabled && volumeFogColorChanged) {
        ClampColorValues(volumeFogColor, true);
        if (detachedEdit) {
            editData.volumeFogScatterColor = ColorFromArray(volumeFogColor, true);
            editData.volumeFogScatterColorEnabled = true;
            if (regionScoped) overrideMask.volumeFogScatterColor = true;
            editChanged = true;
        } else {
            g_oVolumeFogScatterColor.set(volumeFogColor[0], volumeFogColor[1], volumeFogColor[2], volumeFogColor[3]);
        }
    }

    bool fogFromWindChanged = false;
    bool fogFromWindOverrideChanged = false;
    const SliderRange fogFromWindRange = ActiveSliderRange(0.0f, 15.0f, 0.0f, 50.0f);
    if (DrawSliderFloatRow("Fog", "fog_from_wind", &fogFromWind, fogFromWindRange.lo, fogFromWindRange.hi, "%.2f", &fogFromWindChanged, regionScoped ? &overrideMask.nativeFog : nullptr, &fogFromWindOverrideChanged, fogFromWindNative)) {
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
            editData.nativeFog = ClampSliderValue(fogFromWind, fogFromWindRange);
            editData.nativeFogEnabled = fabsf(editData.nativeFog - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.nativeFog = true;
            editChanged = true;
        } else {
            fogFromWind = ClampSliderValue(fogFromWind, fogFromWindRange);
            if (fabsf(fogFromWind - 1.0f) > 0.001f) {
                g_oNativeFog.set(fogFromWind);
            } else {
                g_oNativeFog.clear();
            }
        }
    }

    float aerosolHeight = detachedEdit ? editData.mieScaleHeight : (g_oMieScaleHeight.active.load() ? g_oMieScaleHeight.value.load() : g_windPackBase10.load());
    const bool aerosolHeightNative = detachedEdit ? !editData.mieScaleHeightEnabled : !g_oMieScaleHeight.active.load();
    bool aerosolHeightChanged = false;
    bool aerosolHeightOverrideChanged = false;
    const SliderRange aerosolHeightRange = ActiveSliderRange(10.0f, 20000.0f, 1.0f, 200000.0f);
    if (DrawSliderFloatRow("Aerosol Height", "mie_scale_height", &aerosolHeight, aerosolHeightRange.lo, aerosolHeightRange.hi, "%.1f", &aerosolHeightChanged, regionScoped ? &overrideMask.mieScaleHeight : nullptr, &aerosolHeightOverrideChanged, aerosolHeightNative)) {
        if (detachedEdit) {
            editData.mieScaleHeightEnabled = false;
            editData.mieScaleHeight = g_windPackBase10.load();
            if (regionScoped) overrideMask.mieScaleHeight = true;
            editChanged = true;
        } else {
            g_oMieScaleHeight.clear();
        }
    } else if (aerosolHeightOverrideChanged) {
        editChanged = true;
    } else if (windEnabled && aerosolHeightChanged) {
        if (detachedEdit) {
            editData.mieScaleHeight = ClampSliderValue(aerosolHeight, aerosolHeightRange);
            editData.mieScaleHeightEnabled = true;
            if (regionScoped) overrideMask.mieScaleHeight = true;
            editChanged = true;
        } else {
            g_oMieScaleHeight.set(ClampSliderValue(aerosolHeight, aerosolHeightRange));
        }
    }

    float aerosolDensity = detachedEdit ? editData.mieAerosolDensity : (g_oMieAerosolDensity.active.load() ? g_oMieAerosolDensity.value.load() : g_windPackBase11.load());
    const bool aerosolDensityNative = detachedEdit ? !editData.mieAerosolDensityEnabled : !g_oMieAerosolDensity.active.load();
    bool aerosolDensityChanged = false;
    bool aerosolDensityOverrideChanged = false;
    const SliderRange aerosolDensityRange = ActiveSliderRange(0.0f, 20.0f, 0.0f, 100.0f);
    if (DrawSliderFloatRow("Aerosol Density", "mie_aerosol_density", &aerosolDensity, aerosolDensityRange.lo, aerosolDensityRange.hi, "%.4f", &aerosolDensityChanged, regionScoped ? &overrideMask.mieAerosolDensity : nullptr, &aerosolDensityOverrideChanged, aerosolDensityNative)) {
        if (detachedEdit) {
            editData.mieAerosolDensityEnabled = false;
            editData.mieAerosolDensity = g_windPackBase11.load();
            if (regionScoped) overrideMask.mieAerosolDensity = true;
            editChanged = true;
        } else {
            g_oMieAerosolDensity.clear();
        }
    } else if (aerosolDensityOverrideChanged) {
        editChanged = true;
    } else if (windEnabled && aerosolDensityChanged) {
        if (detachedEdit) {
            editData.mieAerosolDensity = ClampSliderValue(aerosolDensity, aerosolDensityRange);
            editData.mieAerosolDensityEnabled = true;
            if (regionScoped) overrideMask.mieAerosolDensity = true;
            editChanged = true;
        } else {
            g_oMieAerosolDensity.set(ClampSliderValue(aerosolDensity, aerosolDensityRange));
        }
    }

    float aerosolAbsorption = detachedEdit ? editData.mieAerosolAbsorption : (g_oMieAerosolAbsorption.active.load() ? g_oMieAerosolAbsorption.value.load() : g_windPackBase12.load());
    const bool aerosolAbsorptionNative = detachedEdit ? !editData.mieAerosolAbsorptionEnabled : !g_oMieAerosolAbsorption.active.load();
    bool aerosolAbsorptionChanged = false;
    bool aerosolAbsorptionOverrideChanged = false;
    const SliderRange aerosolAbsorptionRange = ActiveSliderRange(0.0f, 5.0f, 0.0f, 100.0f);
    if (DrawSliderFloatRow("Aerosol Absorption", "mie_aerosol_absorption", &aerosolAbsorption, aerosolAbsorptionRange.lo, aerosolAbsorptionRange.hi, "%.4f", &aerosolAbsorptionChanged, regionScoped ? &overrideMask.mieAerosolAbsorption : nullptr, &aerosolAbsorptionOverrideChanged, aerosolAbsorptionNative)) {
        if (detachedEdit) {
            editData.mieAerosolAbsorptionEnabled = false;
            editData.mieAerosolAbsorption = g_windPackBase12.load();
            if (regionScoped) overrideMask.mieAerosolAbsorption = true;
            editChanged = true;
        } else {
            g_oMieAerosolAbsorption.clear();
        }
    } else if (aerosolAbsorptionOverrideChanged) {
        editChanged = true;
    } else if (windEnabled && aerosolAbsorptionChanged) {
        if (detachedEdit) {
            editData.mieAerosolAbsorption = ClampSliderValue(aerosolAbsorption, aerosolAbsorptionRange);
            editData.mieAerosolAbsorptionEnabled = true;
            if (regionScoped) overrideMask.mieAerosolAbsorption = true;
            editChanged = true;
        } else {
            g_oMieAerosolAbsorption.set(ClampSliderValue(aerosolAbsorption, aerosolAbsorptionRange));
        }
    }

    float fogBaseline = detachedEdit ? editData.heightFogBaseline : (g_oHeightFogBaseline.active.load() ? g_oHeightFogBaseline.value.load() : g_windPackBase18.load());
    const bool fogBaselineNative = detachedEdit ? !editData.heightFogBaselineEnabled : !g_oHeightFogBaseline.active.load();
    bool fogBaselineChanged = false;
    bool fogBaselineOverrideChanged = false;
    const SliderRange fogBaselineRange = ActiveSliderRange(-5000.0f, 5000.0f, -50000.0f, 50000.0f);
    if (DrawSliderFloatRow("Fog Height Baseline", "height_fog_baseline", &fogBaseline, fogBaselineRange.lo, fogBaselineRange.hi, "%.1f", &fogBaselineChanged, regionScoped ? &overrideMask.heightFogBaseline : nullptr, &fogBaselineOverrideChanged, fogBaselineNative)) {
        if (detachedEdit) {
            editData.heightFogBaselineEnabled = false;
            editData.heightFogBaseline = g_windPackBase18.load();
            if (regionScoped) overrideMask.heightFogBaseline = true;
            editChanged = true;
        } else {
            g_oHeightFogBaseline.clear();
        }
    } else if (fogBaselineOverrideChanged) {
        editChanged = true;
    } else if (windEnabled && fogBaselineChanged) {
        if (detachedEdit) {
            editData.heightFogBaseline = ClampSliderValue(fogBaseline, fogBaselineRange);
            editData.heightFogBaselineEnabled = true;
            if (regionScoped) overrideMask.heightFogBaseline = true;
            editChanged = true;
        } else {
            g_oHeightFogBaseline.set(ClampSliderValue(fogBaseline, fogBaselineRange));
        }
    }

    float fogFalloff = detachedEdit ? editData.heightFogFalloff : (g_oHeightFogFalloff.active.load() ? g_oHeightFogFalloff.value.load() : g_windPackBase19.load());
    const bool fogFalloffNative = detachedEdit ? !editData.heightFogFalloffEnabled : !g_oHeightFogFalloff.active.load();
    bool fogFalloffChanged = false;
    bool fogFalloffOverrideChanged = false;
    const SliderRange fogFalloffRange = ActiveSliderRange(0.0f, 5.0f, 0.0f, 100.0f);
    if (DrawSliderFloatRow("Fog Height Falloff", "height_fog_falloff", &fogFalloff, fogFalloffRange.lo, fogFalloffRange.hi, "%.4f", &fogFalloffChanged, regionScoped ? &overrideMask.heightFogFalloff : nullptr, &fogFalloffOverrideChanged, fogFalloffNative)) {
        if (detachedEdit) {
            editData.heightFogFalloffEnabled = false;
            editData.heightFogFalloff = g_windPackBase19.load();
            if (regionScoped) overrideMask.heightFogFalloff = true;
            editChanged = true;
        } else {
            g_oHeightFogFalloff.clear();
        }
    } else if (fogFalloffOverrideChanged) {
        editChanged = true;
    } else if (windEnabled && fogFalloffChanged) {
        if (detachedEdit) {
            editData.heightFogFalloff = ClampSliderValue(fogFalloff, fogFalloffRange);
            editData.heightFogFalloffEnabled = true;
            if (regionScoped) overrideMask.heightFogFalloff = true;
            editChanged = true;
        } else {
            g_oHeightFogFalloff.set(ClampSliderValue(fogFalloff, fogFalloffRange));
        }
    }

    if (!windEnabled) {
        ImGui::EndDisabled();
        if (!fogBlocked) {
            if (!RuntimeFeatureAvailable(RuntimeFeatureId::WindControls)) {
                DrawFeatureUnavailable(RuntimeFeatureId::WindControls);
            } else {
                DrawHookUnavailable(RuntimeHookId::WindPack);
            }
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
    const bool enabled = !(detachedEdit ? editData.forceClearSky : g_forceClear.load()) &&
                         RuntimeFeatureAvailable(RuntimeFeatureId::ExperimentControls) &&
                         WindPackReady();
    if (!enabled) {
        ImGui::BeginDisabled();
    }

    float value2C = detachedEdit ? editData.exp2C : (g_oExpCloud2C.active.load() ? g_oExpCloud2C.value.load() : 1.0f);
    const bool value2CNative = detachedEdit ? !editData.exp2CEnabled : !g_oExpCloud2C.active.load();
    bool value2CChanged = false;
    bool value2COverrideChanged = false;
    const SliderRange value2CRange = ActiveSliderRange(0.0f, 15.0f, 0.0f, 50.0f);
    if (DrawSliderFloatRow("2C", "2c", &value2C, value2CRange.lo, value2CRange.hi, "x%.2f", &value2CChanged, regionScoped ? &overrideMask.exp2C : nullptr, &value2COverrideChanged, value2CNative)) {
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
            editData.exp2C = ClampSliderValue(value2C, value2CRange);
            editData.exp2CEnabled = fabsf(editData.exp2C - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.exp2C = true;
            editChanged = true;
        } else {
            value2C = ClampSliderValue(value2C, value2CRange);
            fabsf(value2C - 1.0f) <= 0.001f ? g_oExpCloud2C.clear() : g_oExpCloud2C.set(value2C);
        }
    }

    float value2D = detachedEdit ? editData.exp2D : (g_oExpCloud2D.active.load() ? g_oExpCloud2D.value.load() : 1.0f);
    const bool value2DNative = detachedEdit ? !editData.exp2DEnabled : !g_oExpCloud2D.active.load();
    bool value2DChanged = false;
    bool value2DOverrideChanged = false;
    const SliderRange value2DRange = ActiveSliderRange(0.0f, 15.0f, 0.0f, 50.0f);
    if (DrawSliderFloatRow("2D", "2d", &value2D, value2DRange.lo, value2DRange.hi, "x%.2f", &value2DChanged, regionScoped ? &overrideMask.exp2D : nullptr, &value2DOverrideChanged, value2DNative)) {
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
            editData.exp2D = ClampSliderValue(value2D, value2DRange);
            editData.exp2DEnabled = fabsf(editData.exp2D - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.exp2D = true;
            editChanged = true;
        } else {
            value2D = ClampSliderValue(value2D, value2DRange);
            fabsf(value2D - 1.0f) <= 0.001f ? g_oExpCloud2D.clear() : g_oExpCloud2D.set(value2D);
        }
    }

    float cloudVariation = detachedEdit ? editData.cloudVariation : (g_oCloudVariation.active.load() ? g_oCloudVariation.value.load() : 1.0f);
    const bool cloudVariationNative = detachedEdit ? !editData.cloudVariationEnabled : !g_oCloudVariation.active.load();
    bool cloudVariationChanged = false;
    bool cloudVariationOverrideChanged = false;
    const SliderRange cloudVariationRange = ActiveSliderRange(0.0f, 15.0f, 0.0f, 50.0f);
    if (DrawSliderFloatRow("Cloud Variation [32]", "cloud_variation", &cloudVariation, cloudVariationRange.lo, cloudVariationRange.hi, "x%.2f", &cloudVariationChanged, regionScoped ? &overrideMask.cloudVariation : nullptr, &cloudVariationOverrideChanged, cloudVariationNative)) {
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
            editData.cloudVariation = ClampSliderValue(cloudVariation, cloudVariationRange);
            editData.cloudVariationEnabled = fabsf(editData.cloudVariation - 1.0f) > 0.001f;
            if (regionScoped) overrideMask.cloudVariation = true;
            editChanged = true;
        } else {
            cloudVariation = ClampSliderValue(cloudVariation, cloudVariationRange);
            fabsf(cloudVariation - 1.0f) <= 0.001f ? g_oCloudVariation.clear() : g_oCloudVariation.set(cloudVariation);
        }
    }

    if (!enabled) {
        ImGui::EndDisabled();
        if (!RuntimeFeatureAvailable(RuntimeFeatureId::ExperimentControls)) {
            DrawFeatureUnavailable(RuntimeFeatureId::ExperimentControls);
        } else if (!(detachedEdit ? editData.forceClearSky : g_forceClear.load())) {
            DrawHookUnavailable(RuntimeHookId::WindPack);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Legacy Fog");
    float fogPct = detachedEdit ? editData.fogPercent : 0.0f;
    if (!detachedEdit && g_oFog.active.load()) {
        const float fogN = sqrtf(max(0.0f, g_oFog.value.load() / 100.0f));
        fogPct = fogN * 100.0f;
    }

    const bool fogBlocked = detachedEdit ? (editData.forceClearSky || editData.noFog) : (g_forceClear.load() || g_noFog.load());
    const bool fogFeatureAvailable = !fogBlocked &&
                                     RuntimeFeatureAvailable(RuntimeFeatureId::FogControls) &&
                                     WeatherFrameReady();
    if (!fogFeatureAvailable) {
        ImGui::BeginDisabled();
    }
    const bool fogNative = detachedEdit ? !editData.fogEnabled : !g_oFog.active.load();
    bool fogChanged = false;
    bool fogOverrideChanged = false;
    const SliderRange fogPctRange = ActiveSliderRange(0.0f, 100.0f, 0.0f, 500.0f);
    const bool fogReset = DrawSliderFloatRow("Fog [LEGACY]", "fog_legacy", &fogPct, fogPctRange.lo, fogPctRange.hi, "%.1f%%", &fogChanged, regionScoped ? &overrideMask.fog : nullptr, &fogOverrideChanged, fogNative);

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
            editData.fogPercent = ClampSliderValue(fogPct, fogPctRange);
            editData.fogEnabled = editData.fogPercent > 0.001f;
            if (regionScoped) overrideMask.fog = true;
            editChanged = true;
        } else {
            fogPct = ClampSliderValue(fogPct, fogPctRange);
            const float t = fogPct * 0.01f;
            g_oFog.set(t * t * 100.0f);
        }
    }

    if (!fogFeatureAvailable) {
        ImGui::EndDisabled();
        if (!fogBlocked) {
            if (!RuntimeFeatureAvailable(RuntimeFeatureId::FogControls)) {
                DrawFeatureUnavailable(RuntimeFeatureId::FogControls);
            } else {
                DrawHookUnavailable(RuntimeHookId::WeatherFrameUpdate);
            }
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Details");
    float puddleScale = detachedEdit ? editData.puddleScale : (g_oCloudThk.active.load() ? g_oCloudThk.value.load() : 0.0f);
    const bool puddleScaleNative = detachedEdit ? !editData.puddleScaleEnabled : !g_oCloudThk.active.load();
    const bool detailEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::DetailControls) && WeatherTickReady();
    if (!detailEnabled) {
        ImGui::BeginDisabled();
    }
    bool puddleScaleChanged = false;
    bool puddleScaleOverrideChanged = false;
    const SliderRange puddleScaleRange = ActiveSliderRange(0.0f, 1.0f, 0.0f, 5.0f);
    if (DrawSliderFloatRow("Puddle Scale", "puddle", &puddleScale, puddleScaleRange.lo, puddleScaleRange.hi, "%.3f", &puddleScaleChanged, regionScoped ? &overrideMask.puddleScale : nullptr, &puddleScaleOverrideChanged, puddleScaleNative)) {
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
            editData.puddleScale = ClampSliderValue(puddleScale, puddleScaleRange);
            editData.puddleScaleEnabled = editData.puddleScale > 0.001f;
            if (regionScoped) overrideMask.puddleScale = true;
            editChanged = true;
        } else {
            puddleScale = ClampSliderValue(puddleScale, puddleScaleRange);
            g_oCloudThk.set(puddleScale);
        }
    }
    if (!detailEnabled) {
        ImGui::EndDisabled();
        if (!RuntimeFeatureAvailable(RuntimeFeatureId::DetailControls)) {
            DrawFeatureUnavailable(RuntimeFeatureId::DetailControls);
        } else {
            DrawHookUnavailable(RuntimeHookId::WeatherTick);
        }
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
    const bool celestialWindPackReady = celestialEnabled && WindPackReady();
    const bool celestialSceneFrameReady = celestialEnabled && SceneFrameReady();

    int milkywayTexture = detachedEdit
        ? (editData.milkywayTextureEnabled ? MilkywayTextureFindOptionByName(editData.milkywayTexture.c_str()) : 0)
        : MilkywayTextureSelectedOption();
    if (milkywayTexture < 0) {
        milkywayTexture = 0;
    }
    const bool milkywayTextureReady = MilkywayTextureReady();
    const bool milkywayTextureOverrideChanged = DrawOverrideToggle(regionScoped ? &overrideMask.milkywayTexture : nullptr);
    const bool milkywayTextureRegionOverride = !regionScoped || overrideMask.milkywayTexture;
    if (!milkywayTextureRegionOverride) {
        ImGui::BeginDisabled();
    }
    ImGui::TextUnformatted("Milky Way Texture");
    if (!milkywayTextureRegionOverride) {
        ImGui::EndDisabled();
    }
    if (regionScoped) {
        DrawOverrideBadge(overrideMask.milkywayTexture);
    }
    ImGui::SameLine();
    const float milkywayRefreshWidth = 64.0f;
    const float milkywayRefreshX = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - milkywayRefreshWidth;
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), milkywayRefreshX));
    if (ImGui::Button("Refresh##milkyway", ImVec2(milkywayRefreshWidth, 0.0f))) {
        MilkywayTextureRefreshList();
    }
    char inheritedMilkywayTexture[192] = {};
    const char* milkywayTexturePreview = MilkywayTextureOptionName(milkywayTexture);
    if (!milkywayTextureRegionOverride) {
        sprintf_s(inheritedMilkywayTexture, sizeof(inheritedMilkywayTexture), "G: %s", milkywayTexturePreview);
        milkywayTexturePreview = inheritedMilkywayTexture;
    }
    ImGui::TextDisabled("Selected: %s", milkywayTexturePreview);

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("Browse Milky Way Textures##milkyway_texture_browser")) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##milkyway_texture_filter", "Search...", g_milkywayTextureFilter, IM_ARRAYSIZE(g_milkywayTextureFilter));
        if (!milkywayTextureReady || !milkywayTextureRegionOverride) {
            ImGui::BeginDisabled();
        }
        const float milkywayListHeight = min(220.0f, max(112.0f, ImGui::GetTextLineHeightWithSpacing() * 9.0f));
        if (ImGui::BeginChild("MilkywayTextureLibrary", ImVec2(0.0f, milkywayListHeight), true)) {
            const int optionCount = MilkywayTextureOptionCount();
            const char* currentPack = nullptr;
            int visibleCount = 0;
            for (int i = 0; i < optionCount; ++i) {
                const char* optionName = MilkywayTextureOptionName(i);
                const char* optionLabel = MilkywayTextureOptionLabel(i);
                const bool visible = i == 0
                    ? TextContainsNoCase("Native", g_milkywayTextureFilter)
                    : (TextContainsNoCase(optionLabel, g_milkywayTextureFilter) ||
                       TextContainsNoCase(optionName, g_milkywayTextureFilter) ||
                       TextContainsNoCase(MilkywayTextureOptionPack(i), g_milkywayTextureFilter));
                if (!visible) {
                    continue;
                }
                if (i > 0) {
                    const char* pack = MilkywayTextureOptionPack(i);
                    const char* group = (pack && pack[0]) ? pack : "Loose";
                    if (!currentPack || strcmp(currentPack, group) != 0) {
                        DrawTexturePackHeader(group);
                        currentPack = group;
                    }
                }
                ++visibleCount;
                const bool selected = i == milkywayTexture;
                if (i > 0) {
                    ImGui::Indent(12.0f);
                }
                if (ImGui::Selectable(optionLabel, selected)) {
                    milkywayTexture = i;
                    if (detachedEdit) {
                        editData.milkywayTextureEnabled = i > 0;
                        editData.milkywayTexture = i > 0 ? optionName : "";
                        if (regionScoped) overrideMask.milkywayTexture = true;
                        editChanged = true;
                    } else {
                        MilkywayTextureSelectOption(i);
                    }
                }
                if (i > 0) {
                    ImGui::Unindent(12.0f);
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            if (visibleCount == 0) {
                ImGui::TextDisabled("No Milky Way texture matches");
            }
        }
        ImGui::EndChild();
        if (!milkywayTextureReady || !milkywayTextureRegionOverride) {
            ImGui::EndDisabled();
        }
    }
    if (milkywayTextureOverrideChanged) {
        editChanged = true;
    }

    float nightSkyPitch = detachedEdit
        ? (editData.nightSkyRotationEnabled ? editData.nightSkyRotation : nativeNightSkyPitch)
        : (g_oExpNightSkyRot.active.load() ? g_oExpNightSkyRot.value.load() : nativeNightSkyPitch);
    const bool nightSkyPitchNative = detachedEdit ? !editData.nightSkyRotationEnabled : !g_oExpNightSkyRot.active.load();
    bool nightSkyPitchChanged = false;
    bool nightSkyPitchOverrideChanged = false;
    const SliderRange nightSkyPitchRange = ActiveSliderRange(-89.0f, 89.0f, -180.0f, 180.0f);
    if (!celestialWindPackReady) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Night Sky Tilt", "night_sky_tilt", &nightSkyPitch, nightSkyPitchRange.lo, nightSkyPitchRange.hi, "%.2f", &nightSkyPitchChanged, regionScoped ? &overrideMask.nightSkyRotation : nullptr, &nightSkyPitchOverrideChanged, nightSkyPitchNative)) {
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
    } else if (celestialWindPackReady && nightSkyPitchChanged) {
        if (detachedEdit) {
            editData.nightSkyRotation = ClampSliderValue(nightSkyPitch, nightSkyPitchRange);
            editData.nightSkyRotationEnabled = fabsf(editData.nightSkyRotation - nativeNightSkyPitch) > 0.001f;
            if (regionScoped) overrideMask.nightSkyRotation = true;
            editChanged = true;
        } else {
            nightSkyPitch = ClampSliderValue(nightSkyPitch, nightSkyPitchRange);
            fabsf(nightSkyPitch - nativeNightSkyPitch) <= 0.001f ? g_oExpNightSkyRot.clear() : g_oExpNightSkyRot.set(nightSkyPitch);
        }
    }
    if (!celestialWindPackReady) {
        ImGui::EndDisabled();
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::WindPack);
        }
    }

    float nightSkyYaw = detachedEdit
        ? (editData.nightSkyYawEnabled ? editData.nightSkyYaw : nativeNightSkyYaw)
        : (g_oNightSkyYaw.active.load() ? g_oNightSkyYaw.value.load() : nativeNightSkyYaw);
    const bool nightSkyYawNative = detachedEdit ? !editData.nightSkyYawEnabled : !g_oNightSkyYaw.active.load();
    bool nightSkyYawChanged = false;
    bool nightSkyYawOverrideChanged = false;
    const SliderRange nightSkyYawRange = ActiveSliderRange(-180.0f, 180.0f, -360.0f, 360.0f);
    if (!celestialSceneFrameReady) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Night Sky Phase", "night_sky_phase", &nightSkyYaw, nightSkyYawRange.lo, nightSkyYawRange.hi, "%.2f", &nightSkyYawChanged, regionScoped ? &overrideMask.nightSkyYaw : nullptr, &nightSkyYawOverrideChanged, nightSkyYawNative)) {
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
    } else if (celestialSceneFrameReady && nightSkyYawChanged) {
        if (detachedEdit) {
            editData.nightSkyYaw = ClampSliderValue(nightSkyYaw, nightSkyYawRange);
            editData.nightSkyYawEnabled = fabsf(editData.nightSkyYaw - nativeNightSkyYaw) > 0.001f;
            if (regionScoped) overrideMask.nightSkyYaw = true;
            editChanged = true;
        } else {
            nightSkyYaw = ClampSliderValue(nightSkyYaw, nightSkyYawRange);
            fabsf(nightSkyYaw - nativeNightSkyYaw) <= 0.001f ? g_oNightSkyYaw.clear() : g_oNightSkyYaw.set(nightSkyYaw);
        }
    }
    if (!celestialSceneFrameReady) {
        ImGui::EndDisabled();
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::SceneFrameUpdate);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Sun");

    float sunLightIntensity = detachedEdit
        ? (editData.sunLightIntensityEnabled ? editData.sunLightIntensity : g_windPackBase00.load())
        : (g_oSunLightIntensity.active.load() ? g_oSunLightIntensity.value.load() : g_windPackBase00.load());
    const bool sunLightIntensityNative = detachedEdit ? !editData.sunLightIntensityEnabled : !g_oSunLightIntensity.active.load();
    bool sunLightIntensityChanged = false;
    bool sunLightIntensityOverrideChanged = false;
    const SliderRange sunLightIntensityRange = ActiveSliderRange(0.0f, 20.0f, 0.0f, 100.0f);
    if (!celestialWindPackReady) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Sun Light Intensity", "sun_light_intensity", &sunLightIntensity, sunLightIntensityRange.lo, sunLightIntensityRange.hi, "%.3f", &sunLightIntensityChanged, regionScoped ? &overrideMask.sunLightIntensity : nullptr, &sunLightIntensityOverrideChanged, sunLightIntensityNative)) {
        if (detachedEdit) {
            editData.sunLightIntensityEnabled = false;
            editData.sunLightIntensity = g_windPackBase00.load();
            if (regionScoped) overrideMask.sunLightIntensity = true;
            editChanged = true;
        } else {
            g_oSunLightIntensity.clear();
        }
    } else if (sunLightIntensityOverrideChanged) {
        editChanged = true;
    } else if (celestialWindPackReady && sunLightIntensityChanged) {
        if (detachedEdit) {
            editData.sunLightIntensity = ClampSliderValue(sunLightIntensity, sunLightIntensityRange);
            editData.sunLightIntensityEnabled = true;
            if (regionScoped) overrideMask.sunLightIntensity = true;
            editChanged = true;
        } else {
            g_oSunLightIntensity.set(ClampSliderValue(sunLightIntensity, sunLightIntensityRange));
        }
    }
    if (!celestialWindPackReady) {
        ImGui::EndDisabled();
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::WindPack);
        }
    }

    float sunSize = detachedEdit
        ? (editData.sunSizeEnabled ? editData.sunSize : nativeSunSize)
        : (g_oSunSize.active.load() ? g_oSunSize.value.load() : nativeSunSize);
    const bool sunSizeNative = detachedEdit ? !editData.sunSizeEnabled : !g_oSunSize.active.load();
    bool sunSizeChanged = false;
    bool sunSizeOverrideChanged = false;
    const SliderRange sunSizeRange = ActiveSliderRange(0.01f, 10.0f, 0.001f, 100.0f);
    if (!celestialWindPackReady) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Sun Size", "sun_size", &sunSize, sunSizeRange.lo, sunSizeRange.hi, "%.3f", &sunSizeChanged, regionScoped ? &overrideMask.sunSize : nullptr, &sunSizeOverrideChanged, sunSizeNative)) {
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
    } else if (celestialWindPackReady && sunSizeChanged) {
        if (detachedEdit) {
            editData.sunSize = ClampSliderValue(sunSize, sunSizeRange);
            editData.sunSizeEnabled = fabsf(editData.sunSize - nativeSunSize) > 0.001f;
            if (regionScoped) overrideMask.sunSize = true;
            editChanged = true;
        } else {
            sunSize = ClampSliderValue(sunSize, sunSizeRange);
            fabsf(sunSize - nativeSunSize) <= 0.001f ? g_oSunSize.clear() : g_oSunSize.set(sunSize);
        }
    }
    if (!celestialWindPackReady) {
        ImGui::EndDisabled();
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::WindPack);
        }
    }

    float sunYaw = detachedEdit
        ? (editData.sunYawEnabled ? editData.sunYaw : nativeSunYaw)
        : (g_oSunDirX.active.load() ? g_oSunDirX.value.load() : nativeSunYaw);
    const bool sunYawNative = detachedEdit ? !editData.sunYawEnabled : !g_oSunDirX.active.load();
    bool sunYawChanged = false;
    bool sunYawOverrideChanged = false;
    const SliderRange sunYawRange = ActiveSliderRange(-180.0f, 180.0f, -360.0f, 360.0f);
    if (!celestialSceneFrameReady) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Sun Yaw Lock", "sun_yaw", &sunYaw, sunYawRange.lo, sunYawRange.hi, "%.2f", &sunYawChanged, regionScoped ? &overrideMask.sunYaw : nullptr, &sunYawOverrideChanged, sunYawNative)) {
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
    } else if (celestialSceneFrameReady && sunYawChanged) {
        if (detachedEdit) {
            editData.sunYaw = ClampSliderValue(sunYaw, sunYawRange);
            editData.sunYawEnabled = fabsf(editData.sunYaw - nativeSunYaw) > 0.001f;
            if (regionScoped) overrideMask.sunYaw = true;
            editChanged = true;
        } else {
            sunYaw = ClampSliderValue(sunYaw, sunYawRange);
            fabsf(sunYaw - nativeSunYaw) <= 0.001f ? g_oSunDirX.clear() : g_oSunDirX.set(sunYaw);
        }
    }
    if (!celestialSceneFrameReady) {
        ImGui::EndDisabled();
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::SceneFrameUpdate);
        }
    }

    float sunPitch = detachedEdit
        ? (editData.sunPitchEnabled ? editData.sunPitch : nativeSunPitch)
        : (g_oSunDirY.active.load() ? g_oSunDirY.value.load() : nativeSunPitch);
    const bool sunPitchNative = detachedEdit ? !editData.sunPitchEnabled : !g_oSunDirY.active.load();
    bool sunPitchChanged = false;
    bool sunPitchOverrideChanged = false;
    const SliderRange sunPitchRange = ActiveSliderRange(-89.0f, 89.0f, -180.0f, 180.0f);
    if (!celestialSceneFrameReady) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Sun Pitch Lock", "sun_pitch", &sunPitch, sunPitchRange.lo, sunPitchRange.hi, "%.2f", &sunPitchChanged, regionScoped ? &overrideMask.sunPitch : nullptr, &sunPitchOverrideChanged, sunPitchNative)) {
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
    } else if (celestialSceneFrameReady && sunPitchChanged) {
        if (detachedEdit) {
            editData.sunPitch = ClampSliderValue(sunPitch, sunPitchRange);
            editData.sunPitchEnabled = fabsf(editData.sunPitch - nativeSunPitch) > 0.001f;
            if (regionScoped) overrideMask.sunPitch = true;
            editChanged = true;
        } else {
            sunPitch = ClampSliderValue(sunPitch, sunPitchRange);
            fabsf(sunPitch - nativeSunPitch) <= 0.001f ? g_oSunDirY.clear() : g_oSunDirY.set(sunPitch);
        }
    }
    if (!celestialSceneFrameReady) {
        ImGui::EndDisabled();
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::SceneFrameUpdate);
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

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("Browse Moon Textures##moon_texture_browser")) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##moon_texture_filter", "Search...", g_moonTextureFilter, IM_ARRAYSIZE(g_moonTextureFilter));
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
                        DrawTexturePackHeader(group);
                        currentPack = group;
                    }
                }
                ++visibleCount;
                const bool selected = i == moonTexture;
                if (i > 0) {
                    ImGui::Indent(12.0f);
                }
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
                if (i > 0) {
                    ImGui::Unindent(12.0f);
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
    }
    if (moonTextureOverrideChanged) {
        editChanged = true;
    }

    float moonLightIntensity = detachedEdit
        ? (editData.moonLightIntensityEnabled ? editData.moonLightIntensity : g_windPackBase05.load())
        : (g_oMoonLightIntensity.active.load() ? g_oMoonLightIntensity.value.load() : g_windPackBase05.load());
    const bool moonLightIntensityNative = detachedEdit ? !editData.moonLightIntensityEnabled : !g_oMoonLightIntensity.active.load();
    bool moonLightIntensityChanged = false;
    bool moonLightIntensityOverrideChanged = false;
    const SliderRange moonLightIntensityRange = ActiveSliderRange(0.0f, 20.0f, 0.0f, 100.0f);
    if (!celestialWindPackReady) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Moon Light Intensity", "moon_light_intensity", &moonLightIntensity, moonLightIntensityRange.lo, moonLightIntensityRange.hi, "%.3f", &moonLightIntensityChanged, regionScoped ? &overrideMask.moonLightIntensity : nullptr, &moonLightIntensityOverrideChanged, moonLightIntensityNative)) {
        if (detachedEdit) {
            editData.moonLightIntensityEnabled = false;
            editData.moonLightIntensity = g_windPackBase05.load();
            if (regionScoped) overrideMask.moonLightIntensity = true;
            editChanged = true;
        } else {
            g_oMoonLightIntensity.clear();
        }
    } else if (moonLightIntensityOverrideChanged) {
        editChanged = true;
    } else if (celestialWindPackReady && moonLightIntensityChanged) {
        if (detachedEdit) {
            editData.moonLightIntensity = ClampSliderValue(moonLightIntensity, moonLightIntensityRange);
            editData.moonLightIntensityEnabled = true;
            if (regionScoped) overrideMask.moonLightIntensity = true;
            editChanged = true;
        } else {
            g_oMoonLightIntensity.set(ClampSliderValue(moonLightIntensity, moonLightIntensityRange));
        }
    }
    if (!celestialWindPackReady) {
        ImGui::EndDisabled();
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::WindPack);
        }
    }

    float moonSize = detachedEdit
        ? (editData.moonSizeEnabled ? editData.moonSize : nativeMoonSize)
        : (g_oMoonSize.active.load() ? g_oMoonSize.value.load() : nativeMoonSize);
    const bool moonSizeNative = detachedEdit ? !editData.moonSizeEnabled : !g_oMoonSize.active.load();
    bool moonSizeChanged = false;
    bool moonSizeOverrideChanged = false;
    const SliderRange moonSizeRange = ActiveSliderRange(0.020f, 20.0f, 0.001f, 100.0f);
    if (!celestialWindPackReady) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Moon Size", "moon_size", &moonSize, moonSizeRange.lo, moonSizeRange.hi, "%.3f", &moonSizeChanged, regionScoped ? &overrideMask.moonSize : nullptr, &moonSizeOverrideChanged, moonSizeNative)) {
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
    } else if (celestialWindPackReady && moonSizeChanged) {
        if (detachedEdit) {
            editData.moonSize = ClampSliderValue(moonSize, moonSizeRange);
            editData.moonSizeEnabled = fabsf(editData.moonSize - nativeMoonSize) > 0.001f;
            if (regionScoped) overrideMask.moonSize = true;
            editChanged = true;
        } else {
            moonSize = ClampSliderValue(moonSize, moonSizeRange);
            fabsf(moonSize - nativeMoonSize) <= 0.001f ? g_oMoonSize.clear() : g_oMoonSize.set(moonSize);
        }
    }
    if (!celestialWindPackReady) {
        ImGui::EndDisabled();
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::WindPack);
        }
    }

    float moonYaw = detachedEdit
        ? (editData.moonYawEnabled ? editData.moonYaw : nativeMoonYaw)
        : (g_oMoonDirX.active.load() ? g_oMoonDirX.value.load() : nativeMoonYaw);
    const bool moonYawNative = detachedEdit ? !editData.moonYawEnabled : !g_oMoonDirX.active.load();
    bool moonYawChanged = false;
    bool moonYawOverrideChanged = false;
    const SliderRange moonYawRange = ActiveSliderRange(-180.0f, 180.0f, -360.0f, 360.0f);
    if (!celestialSceneFrameReady) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Moon Yaw Lock", "moon_yaw", &moonYaw, moonYawRange.lo, moonYawRange.hi, "%.2f", &moonYawChanged, regionScoped ? &overrideMask.moonYaw : nullptr, &moonYawOverrideChanged, moonYawNative)) {
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
    } else if (celestialSceneFrameReady && moonYawChanged) {
        if (detachedEdit) {
            editData.moonYaw = ClampSliderValue(moonYaw, moonYawRange);
            editData.moonYawEnabled = fabsf(editData.moonYaw - nativeMoonYaw) > 0.001f;
            if (regionScoped) overrideMask.moonYaw = true;
            editChanged = true;
        } else {
            moonYaw = ClampSliderValue(moonYaw, moonYawRange);
            fabsf(moonYaw - nativeMoonYaw) <= 0.001f ? g_oMoonDirX.clear() : g_oMoonDirX.set(moonYaw);
        }
    }
    if (!celestialSceneFrameReady) {
        ImGui::EndDisabled();
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::SceneFrameUpdate);
        }
    }

    float moonPitch = detachedEdit
        ? (editData.moonPitchEnabled ? editData.moonPitch : nativeMoonPitch)
        : (g_oMoonDirY.active.load() ? g_oMoonDirY.value.load() : nativeMoonPitch);
    const bool moonPitchNative = detachedEdit ? !editData.moonPitchEnabled : !g_oMoonDirY.active.load();
    bool moonPitchChanged = false;
    bool moonPitchOverrideChanged = false;
    const SliderRange moonPitchRange = ActiveSliderRange(-89.0f, 89.0f, -180.0f, 180.0f);
    if (!celestialSceneFrameReady) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Moon Pitch Lock", "moon_pitch", &moonPitch, moonPitchRange.lo, moonPitchRange.hi, "%.2f", &moonPitchChanged, regionScoped ? &overrideMask.moonPitch : nullptr, &moonPitchOverrideChanged, moonPitchNative)) {
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
    } else if (celestialSceneFrameReady && moonPitchChanged) {
        if (detachedEdit) {
            editData.moonPitch = ClampSliderValue(moonPitch, moonPitchRange);
            editData.moonPitchEnabled = fabsf(editData.moonPitch - nativeMoonPitch) > 0.001f;
            if (regionScoped) overrideMask.moonPitch = true;
            editChanged = true;
        } else {
            moonPitch = ClampSliderValue(moonPitch, moonPitchRange);
            fabsf(moonPitch - nativeMoonPitch) <= 0.001f ? g_oMoonDirY.clear() : g_oMoonDirY.set(moonPitch);
        }
    }
    if (!celestialSceneFrameReady) {
        ImGui::EndDisabled();
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::SceneFrameUpdate);
        }
    }

    float moonRoll = detachedEdit
        ? (editData.moonRollEnabled ? editData.moonRoll : nativeMoonRoll)
        : (g_oMoonRoll.active.load() ? g_oMoonRoll.value.load() : nativeMoonRoll);
    const bool moonRollNative = detachedEdit ? !editData.moonRollEnabled : !g_oMoonRoll.active.load();
    bool moonRollChanged = false;
    bool moonRollOverrideChanged = false;
    const SliderRange moonRollRange = ActiveSliderRange(-180.0f, 180.0f, -360.0f, 360.0f);
    if (!celestialSceneFrameReady) {
        ImGui::BeginDisabled();
    }
    if (DrawSliderFloatRow("Moon Rotation", "moon_roll", &moonRoll, moonRollRange.lo, moonRollRange.hi, "%.2f", &moonRollChanged, regionScoped ? &overrideMask.moonRoll : nullptr, &moonRollOverrideChanged, moonRollNative)) {
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
    } else if (celestialSceneFrameReady && moonRollChanged) {
        if (detachedEdit) {
            editData.moonRoll = ClampSliderValue(moonRoll, moonRollRange);
            editData.moonRollEnabled = fabsf(editData.moonRoll - nativeMoonRoll) > 0.001f;
            if (regionScoped) overrideMask.moonRoll = true;
            editChanged = true;
        } else {
            moonRoll = ClampSliderValue(moonRoll, moonRollRange);
            fabsf(moonRoll - nativeMoonRoll) <= 0.001f ? g_oMoonRoll.clear() : g_oMoonRoll.set(moonRoll);
        }
    }
    if (!celestialSceneFrameReady) {
        ImGui::EndDisabled();
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::SceneFrameUpdate);
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

    ImGui::Text("%s %s", MOD_NAME, MOD_VERSION);
    if (g_addonStartupState.load() != AddonStartupState::Ready) {
        ImGui::Spacing();
        DrawStartupGate();
        ImGui::End();
        return;
    }

    ImGui::SameLine();
    DrawEditScopeCombo();
    ImGui::Spacing();

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
#if defined(CW_DEV_BUILD)
        if (ImGui::BeginTabItem("Dev")) {
            DrawDevTab();
            ImGui::EndTabItem();
        }
#endif
        if (ImGui::BeginTabItem("Status")) {
            DrawStatusTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::Text("Unsaved changes: %s", Preset_HasUnsavedChanges() ? "Yes" : "No");
    const char* nexusLabel = "nexusmods";
    const float nexusWidth = ImGui::CalcTextSize(nexusLabel).x;
    const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    ImGui::SameLine();
    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), rightEdge - nexusWidth));
    ImGui::TextLinkOpenURL(nexusLabel, "https://www.nexusmods.com/crimsondesert/mods/632");
    ImGui::End();
#endif
}

static void OnReShadeInitDevice(reshade::api::device* device) {
    if (device->get_api() != reshade::api::device_api::d3d12)
        return;
    Log("[moon-main] init_device event device=%p\n", device->get_native());
    SkyTextureOnInitDevice(
        reinterpret_cast<ID3D12Device*>(device->get_native()));
}

} // namespace

bool InitializeOverlayBridge(HMODULE module) {
    if (!reshade::register_addon(module)) {
        return false;
    }

    reshade::register_overlay(MOD_NAME, &DrawOverlay);

    reshade::register_event<reshade::addon_event::init_device>(&OnReShadeInitDevice);

    g_overlayModule = module;
    g_overlayRegistered = true;
    return true;
}

void ShutdownOverlayBridge() {
    if (!g_overlayRegistered) {
        return;
    }

    reshade::unregister_event<reshade::addon_event::init_device>(&OnReShadeInitDevice);

    reshade::unregister_overlay(MOD_NAME, &DrawOverlay);
    reshade::unregister_addon(g_overlayModule);
    g_overlayRegistered = false;
    g_overlayModule = nullptr;
}
