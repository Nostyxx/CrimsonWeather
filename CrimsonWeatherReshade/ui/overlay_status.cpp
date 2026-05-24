#include "pch.h"

#include "overlay_internal.h"
#include "community_service.h"
#include "sky_texture_override.h"
#include "preset_service.h"
#include "runtime_shared.h"
#include "update_service.h"

#include <imgui.h>
#include <reshade.hpp>

#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

namespace overlay_internal {
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
    bool autoDownloadUpdates = g_cfg.updaterAutoDownload;
    if (ImGui::Checkbox("AutoDownload Updates", &autoDownloadUpdates)) {
        g_cfg.updaterAutoDownload = autoDownloadUpdates;
        SaveGeneralConfig();
        GUI_SetStatus(autoDownloadUpdates ? "AutoDownload enabled" : "AutoDownload disabled");
        if (autoDownloadUpdates) {
            ImGui::OpenPopup("AutoDownload Updates");
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When disabled, Download keeps opening the Nexus Mods page. When enabled, Download installs directly from the Crimson Weather update server.");
    }
    ImGui::SetNextWindowBgAlpha(0.92f);
    if (ImGui::BeginPopupModal("AutoDownload Updates", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("AutoDownload is now enabled.");
        ImGui::Separator();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(
            "When an update is available, Crimson Weather will download the new .addon64 "
            "from the Cloudflare update server directly into your bin64 folder when you press Install Update.");
        ImGui::TextUnformatted(
            "It replaces the local CrimsonWeather.addon64. The new add-on takes effect after restarting Crimson Desert.");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("I understand", ImVec2(140.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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
        DrawStatusRowText(
            snapshot,
            "AutoDownload Updates",
            g_cfg.updaterAutoDownload ? "On" : "Off",
            false,
            g_cfg.updaterAutoDownload
                ? "Download installs verified .addon64 updates directly from the Crimson Weather update server."
                : "Download opens the Nexus Mods page.");
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
            DrawStatusRowBool(snapshot, "Match In-Game Clock", snapshot.effective.visualTimeOverride && snapshot.effective.progressVisualTime && snapshot.effective.progressVisualTimeMatchGameTime, snapshot.regionSource.time, "Progress Visual Time advances only when the in-game HUD clock changes.");
        } else {
            DrawStatusRowHookDisabled(snapshot, "Visual Time Override", RuntimeHookId::WeatherTick);
            DrawStatusRowHookDisabled(snapshot, "Progress Visual Time", RuntimeHookId::WeatherTick);
            DrawStatusRowHookDisabled(snapshot, "Match In-Game Clock", RuntimeHookId::WeatherTick);
        }
        if (!WeatherTickReady()) {
            DrawStatusRowHookDisabled(snapshot, "Advance Interval", RuntimeHookId::WeatherTick);
        } else if (snapshot.effective.visualTimeOverride && snapshot.effective.progressVisualTime) {
            char intervalText[48] = {};
            if (snapshot.effective.progressVisualTimeMatchGameTime) {
                char cadenceText[32] = {};
                FormatProgressVisualTimeInterval(snapshot.effective.progressVisualTimeIntervalMs, cadenceText, sizeof(cadenceText));
                sprintf_s(intervalText, sizeof(intervalText), "Game clock, %s", cadenceText);
            } else {
                FormatProgressVisualTimeInterval(snapshot.effective.progressVisualTimeIntervalMs, intervalText, sizeof(intervalText));
            }
            char tooltipText[160] = {};
            if (snapshot.effective.progressVisualTimeMatchGameTime) {
                strcpy_s(tooltipText, sizeof(tooltipText), "Crimson Weather advances visual time when the in-game HUD clock changes.");
            } else {
                sprintf_s(tooltipText, sizeof(tooltipText), "Crimson Weather currently advances the visual time override at %s.", intervalText);
            }
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
            DrawStatusRowHookDisabled(snapshot, "Cloud Fade Range", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Cloud Detail Ratio", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Cloud Phase Front", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Cloud Scattering Coefficient", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Cloud Flow", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Cloud Visible Range", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Rayleigh Height", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Ozone Ratio", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Rayleigh Scattering Color", RuntimeHookId::WindPack);
        } else if (snapshot.effective.forceClearSky) {
            DrawStatusRowBlocked(snapshot, "Cloud Amount", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud amount overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Height", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud height overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Density", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud density overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Mid Clouds", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so mid-cloud overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "High Clouds", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so high-cloud overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Alpha", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud alpha overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Fade Range", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud fade range overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Detail Ratio", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud detail ratio overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Phase Front", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud phase overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Scattering Coefficient", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud scattering overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Flow", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud flow overrides are not applied.");
            DrawStatusRowBlocked(snapshot, "Cloud Visible Range", snapshot.regionSource.forceClearSky, "Force Clear Sky is active, so cloud visible range overrides are not applied.");
        } else {
            DrawStatusRowEnabledFloat(snapshot, "Cloud Amount", snapshot.effective.cloudAmountEnabled, snapshot.effective.cloudAmount, "x%.2f", snapshot.regionSource.cloudAmount, "Crimson Weather currently multiplies cloud amount by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Height", snapshot.effective.cloudHeightEnabled, snapshot.effective.cloudHeight, "x%.2f", snapshot.regionSource.cloudHeight, "Crimson Weather currently multiplies cloud height by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Density", snapshot.effective.cloudDensityEnabled, snapshot.effective.cloudDensity, "x%.2f", snapshot.regionSource.cloudDensity, "Crimson Weather currently multiplies cloud density by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Mid Clouds", snapshot.effective.midCloudsEnabled, snapshot.effective.midClouds, "x%.2f", snapshot.regionSource.midClouds, "Crimson Weather currently multiplies mid clouds by %s.");
            DrawStatusRowEnabledFloat(snapshot, "High Clouds", snapshot.effective.highCloudsEnabled, snapshot.effective.highClouds, "x%.2f", snapshot.regionSource.highClouds, "Crimson Weather currently multiplies high clouds by %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Alpha", snapshot.effective.cloudAlphaEnabled, snapshot.effective.cloudAlpha, "%.3f", snapshot.regionSource.cloudAlpha, "Crimson Weather currently sets cloud alpha to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Fade Range", snapshot.effective.cloudFadeRangeEnabled, snapshot.effective.cloudFadeRange, "%.1f", snapshot.regionSource.cloudFadeRange, "Crimson Weather currently sets cloud fade range to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Detail Ratio", snapshot.effective.cloudDetailRatioEnabled, snapshot.effective.cloudDetailRatio, "%.4f", snapshot.regionSource.cloudDetailRatio, "Crimson Weather currently sets cloud detail ratio to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Phase Front", snapshot.effective.cloudPhaseFrontEnabled, snapshot.effective.cloudPhaseFront, "%.4f", snapshot.regionSource.cloudPhaseFront, "Crimson Weather currently sets cloud phase front to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Scattering Coefficient", snapshot.effective.cloudScatteringCoefficientEnabled, snapshot.effective.cloudScatteringCoefficient, "%.5f", snapshot.regionSource.cloudScatteringCoefficient, "Crimson Weather currently sets cloud scattering coefficient to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Flow", snapshot.effective.cloudFlowEnabled, snapshot.effective.cloudFlow, "x%.3f", snapshot.regionSource.cloudFlow, "Crimson Weather currently sets cloud flow to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Cloud Visible Range", snapshot.effective.cloudVisibleRangeEnabled, snapshot.effective.cloudVisibleRange, "x%.3f", snapshot.regionSource.cloudVisibleRange, "Crimson Weather currently multiplies cloud visible range by %s.");
        }
        if (WindPackReady()) {
            DrawStatusRowEnabledFloat(snapshot, "Rayleigh Height", snapshot.effective.rayleighHeightEnabled, snapshot.effective.rayleighHeight, "%.1f", snapshot.regionSource.rayleighHeight, "Crimson Weather currently sets Rayleigh height to %s.");
            DrawStatusRowEnabledFloat(snapshot, "Ozone Ratio", snapshot.effective.ozoneRatioEnabled, snapshot.effective.ozoneRatio, "%.4f", snapshot.regionSource.ozoneRatio, "Crimson Weather currently sets ozone ratio to %s.");
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
            DrawStatusRowHookDisabled(snapshot, "Mie Scatter Color", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Aerosol Height", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Aerosol Density", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Aerosol Absorption", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Fog Height Baseline", RuntimeHookId::WindPack);
            DrawStatusRowHookDisabled(snapshot, "Fog Height Falloff", RuntimeHookId::WindPack);
        } else if (fogForcedZero) {
            DrawStatusRowBlocked(snapshot, "Fog", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Volume Fog Scatter Color", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Mie Scatter Color", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Aerosol Height", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Aerosol Density", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Aerosol Absorption", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Fog Height Baseline", fogForceSource, nativeFogForceTooltip);
            DrawStatusRowBlocked(snapshot, "Fog Height Falloff", fogForceSource, nativeFogForceTooltip);
        } else {
            DrawStatusRowEnabledFloat(snapshot, "Fog", snapshot.effective.nativeFogEnabled, snapshot.effective.nativeFog, "%.2f", snapshot.regionSource.nativeFog, "Crimson Weather currently scales native fog by %s.");
            DrawStatusRowText(snapshot, "Volume Fog Scatter Color", snapshot.effective.volumeFogScatterColorEnabled ? "ACTIVE" : "NATIVE", snapshot.regionSource.volumeFogScatterColor, "Crimson Weather currently overrides volume fog scatter color.");
            DrawStatusRowText(snapshot, "Mie Scatter Color", snapshot.effective.mieScatterColorEnabled ? "ACTIVE" : "NATIVE", snapshot.regionSource.mieScatterColor, "Crimson Weather currently overrides Mie scatter color.");
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

} // namespace overlay_internal
