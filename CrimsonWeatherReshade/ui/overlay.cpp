#include "pch.h"

#include "overlay_bridge.h"
#include "preset_service.h"
#include "runtime_shared.h"

#include <imgui.h>
#include <reshade.hpp>

#include <cmath>

namespace {

HMODULE g_overlayModule = nullptr;
bool g_overlayRegistered = false;
char g_newPresetName[128] = "NewPreset.ini";

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
    if (ImGui::Checkbox("Enable Crimson Weather", &enabled)) {
        SetModEnabled(enabled);
    }
    ImGui::Separator();

    ImGui::Text("Selected: %s", Preset_GetSelectedDisplayName());
    ImGui::SetNextItemWidth(300.0f);
    if (ImGui::BeginCombo("Preset", Preset_GetSelectedDisplayName())) {
        const bool newSelected = !Preset_HasSelection();
        if (ImGui::Selectable("[New Preset]", newSelected)) {
            Preset_SelectNew();
        }
        const int count = Preset_GetCount();
        for (int i = 0; i < count; ++i) {
            const bool selected = i == Preset_GetSelectedIndex();
            if (ImGui::Selectable(Preset_GetDisplayName(i), selected)) {
                Preset_SelectIndex(i);
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    if (!Preset_CanSaveCurrent()) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save")) {
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
    if (ImGui::Button("Refresh")) {
        Preset_Refresh();
        GUI_SetStatus("Preset list refreshed");
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset All")) {
        ResetAllSliders();
        g_activeWeather = -1;
        GUI_SetStatus("Sliders reset");
    }

    if (ImGui::BeginPopupModal("Save Preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("File name", g_newPresetName, IM_ARRAYSIZE(g_newPresetName));
        if (ImGui::Button("Create")) {
            Preset_SaveAs(g_newPresetName);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

}

void DrawWeatherTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

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
    ImGui::SliderFloat("Rain", &rain, 0.0f, 1.0f, "%.3f");
    if (DrawResetButton("R##rain")) {
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
    ImGui::SliderFloat("Dust", &dust, 0.0f, 2.0f, "%.3f");
    if (DrawResetButton("R##dust")) {
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
    ImGui::SliderFloat("Snow", &snow, 0.0f, 1.0f, "%.3f");
    if (DrawResetButton("R##snow")) {
        g_oSnow.clear();
    } else if (snowEnabled) {
        snow > 0.0001f ? g_oSnow.set(snow) : g_oSnow.clear();
    }
    if (!snowEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::Snow);
    }

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
    ImGui::SliderInt("Time", &timeMinutes, 0, (24 * 60) - 1, targetClock);
    if (DrawResetButton("R##time")) {
        g_timeTargetHour.store(MinuteOfDayToHour(HourToMinuteOfDay(g_timeCurrentHour.load())));
        g_timeCtrlActive.store(true);
        g_timeFreeze.store(true);
        g_timeApplyRequest.store(true);
    } else if (timeEnabled && visualTimeOverride) {
        g_timeTargetHour.store(MinuteOfDayToHour(timeMinutes));
        g_timeCtrlActive.store(true);
        g_timeFreeze.store(true);
        g_timeApplyRequest.store(true);
    }
    if (!(timeEnabled && visualTimeOverride)) {
        ImGui::EndDisabled();
    }
}

void DrawAtmosphereTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

    const bool cloudEnabled = !g_forceClear.load() && RuntimeFeatureAvailable(RuntimeFeatureId::CloudControls);
    ImGui::TextUnformatted("Clouds");
    ImGui::Separator();
    if (!cloudEnabled) {
        ImGui::BeginDisabled();
    }

    float cloudHeight = g_oCloudSpdX.active.load() ? g_oCloudSpdX.value.load() : 1.0f;
    ImGui::SliderFloat("Cloud Height", &cloudHeight, -20.0f, 20.0f, "%.2f");
    if (DrawResetButton("R##cloud_height")) {
        g_oCloudSpdX.clear();
    } else if (cloudEnabled) {
        g_oCloudSpdX.set(cloudHeight);
    }

    float cloudDensity = g_oCloudSpdY.active.load() ? g_oCloudSpdY.value.load() : 1.0f;
    ImGui::SliderFloat("Cloud Density", &cloudDensity, 0.0f, 10.0f, "x%.2f");
    if (DrawResetButton("R##cloud_density")) {
        g_oCloudSpdY.clear();
    } else if (cloudEnabled) {
        fabsf(cloudDensity - 1.0f) <= 0.001f ? g_oCloudSpdY.clear() : g_oCloudSpdY.set(cloudDensity);
    }

    float midClouds = g_oHighClouds.active.load() ? g_oHighClouds.value.load() : 1.0f;
    ImGui::SliderFloat("Mid Clouds", &midClouds, 0.0f, 15.0f, "x%.2f");
    if (DrawResetButton("R##mid_clouds")) {
        g_oHighClouds.clear();
    } else if (cloudEnabled) {
        fabsf(midClouds - 1.0f) <= 0.001f ? g_oHighClouds.clear() : g_oHighClouds.set(midClouds);
    }

    float highClouds = g_oAtmoAlpha.active.load() ? g_oAtmoAlpha.value.load() : 1.0f;
    ImGui::SliderFloat("High Clouds", &highClouds, 0.0f, 15.0f, "x%.2f");
    if (DrawResetButton("R##high_clouds")) {
        g_oAtmoAlpha.clear();
    } else if (cloudEnabled) {
        fabsf(highClouds - 1.0f) <= 0.001f ? g_oAtmoAlpha.clear() : g_oAtmoAlpha.set(highClouds);
    }

    if (!cloudEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::CloudControls);
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Atmosphere");
    ImGui::Separator();

    float fogPct = 0.0f;
    if (g_oFog.active.load()) {
        const float fogN = sqrtf(min(1.0f, max(0.0f, g_oFog.value.load() / 100.0f)));
        fogPct = fogN * 100.0f;
    }

    const bool fogFeatureAvailable = RuntimeFeatureAvailable(RuntimeFeatureId::FogControls);
    if (!fogFeatureAvailable) {
        ImGui::BeginDisabled();
    }
    ImGui::SliderFloat("Fog [LEGACY]", &fogPct, 0.0f, 100.0f, "%.1f%%");
    const bool fogReset = DrawResetButton("R##fog_legacy");

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
    ImGui::SliderFloat("Fog", &fogFromWind, 0.0f, 15.0f, "%.2f");
    if (DrawResetButton("R##fog_from_wind")) {
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

    float wind = g_windMul.load();
    if (!windEnabled) {
        ImGui::BeginDisabled();
    }
    ImGui::SliderFloat("Wind", &wind, 0.0f, 15.0f, "x%.2f");
    if (DrawResetButton("R##wind_new")) {
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

void DrawExperimentTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

    const bool enabled = !g_forceClear.load() && RuntimeFeatureAvailable(RuntimeFeatureId::ExperimentControls);
    if (!enabled) {
        ImGui::BeginDisabled();
    }

    float value2C = g_oExpCloud2C.active.load() ? g_oExpCloud2C.value.load() : 1.0f;
    ImGui::SliderFloat("2C", &value2C, 0.0f, 15.0f, "x%.2f");
    if (DrawResetButton("R##2c")) {
        g_oExpCloud2C.clear();
    } else if (enabled) {
        fabsf(value2C - 1.0f) <= 0.001f ? g_oExpCloud2C.clear() : g_oExpCloud2C.set(value2C);
    }

    float value2D = g_oExpCloud2D.active.load() ? g_oExpCloud2D.value.load() : 1.0f;
    ImGui::SliderFloat("2D", &value2D, 0.0f, 15.0f, "x%.2f");
    if (DrawResetButton("R##2d")) {
        g_oExpCloud2D.clear();
    } else if (enabled) {
        fabsf(value2D - 1.0f) <= 0.001f ? g_oExpCloud2D.clear() : g_oExpCloud2D.set(value2D);
    }

    float rotation = g_oExpNightSkyRot.active.load() ? g_oExpNightSkyRot.value.load() : 1.0f;
    ImGui::SliderFloat("Night Sky Rotation X [0A]", &rotation, -15.0f, 15.0f, "%.2f");
    if (DrawResetButton("R##rotation")) {
        g_oExpNightSkyRot.clear();
    } else if (enabled) {
        fabsf(rotation - 1.0f) <= 0.001f ? g_oExpNightSkyRot.clear() : g_oExpNightSkyRot.set(rotation);
    }

    float puddleScale = g_oCloudThk.active.load() ? g_oCloudThk.value.load() : 0.0f;
    const bool detailEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::DetailControls);
    if (!detailEnabled) {
        ImGui::BeginDisabled();
    }
    ImGui::SliderFloat("Puddle Scale", &puddleScale, 0.0f, 1.0f, "%.3f");
    if (DrawResetButton("R##puddle")) {
        g_oCloudThk.clear();
    } else if (detailEnabled) {
        g_oCloudThk.set(puddleScale);
    }
    if (!detailEnabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::DetailControls);
    }

    if (!enabled) {
        ImGui::EndDisabled();
        DrawFeatureUnavailable(RuntimeFeatureId::ExperimentControls);
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

    if (ImGui::BeginTabBar("cw_tabs")) {
        if (ImGui::BeginTabItem("Presets")) {
            DrawPresetTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Weather")) {
            DrawWeatherTab();
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
