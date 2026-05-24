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

    DrawSliderById("2c", &editData, &overrideMask, &editChanged);
    DrawSliderById("2d", &editData, &overrideMask, &editChanged);
    DrawSliderById("cloud_variation", &editData, &overrideMask, &editChanged);

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
    const bool fogBlocked = detachedEdit ? (editData.forceClearSky || editData.noFog) : (g_forceClear.load() || g_noFog.load());
    const bool fogFeatureAvailable = !fogBlocked &&
                                     RuntimeFeatureAvailable(RuntimeFeatureId::FogControls) &&
                                     WeatherFrameReady();
    if (!fogFeatureAvailable) {
        ImGui::BeginDisabled();
    }
    DrawSliderById("fog_legacy", &editData, &overrideMask, &editChanged);

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
    const bool detailEnabled = RuntimeFeatureAvailable(RuntimeFeatureId::DetailControls) && WeatherTickReady();
    if (!detailEnabled) {
        ImGui::BeginDisabled();
    }
    DrawSliderById("puddle", &editData, &overrideMask, &editChanged);
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

} // namespace overlay_internal
