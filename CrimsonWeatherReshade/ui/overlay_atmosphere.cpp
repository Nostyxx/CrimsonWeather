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
    DrawControlById("rayleigh_scattering_color", &editData, &overrideMask, &editChanged);

    DrawSliderById("rayleigh_height", &editData, &overrideMask, &editChanged);
    DrawSliderById("ozone_ratio", &editData, &overrideMask, &editChanged);
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

    DrawSliderById("cloud_amount", &editData, &overrideMask, &editChanged);
    DrawSliderById("cloud_height", &editData, &overrideMask, &editChanged);
    DrawSliderById("cloud_density", &editData, &overrideMask, &editChanged);
    DrawSliderById("mid_clouds", &editData, &overrideMask, &editChanged);
    DrawSliderById("high_clouds", &editData, &overrideMask, &editChanged);
    DrawSliderById("cloud_alpha", &editData, &overrideMask, &editChanged);
    DrawSliderById("cloud_fade_range", &editData, &overrideMask, &editChanged);
    DrawSliderById("cloud_detail_ratio", &editData, &overrideMask, &editChanged);
    DrawSliderById("cloud_phase_front", &editData, &overrideMask, &editChanged);
    DrawSliderById("cloud_scattering_coefficient", &editData, &overrideMask, &editChanged);
    DrawSliderById("cloud_flow", &editData, &overrideMask, &editChanged);
    DrawSliderById("cloud_visible_range", &editData, &overrideMask, &editChanged);

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

    DrawControlById("no_fog", &editData, &overrideMask, &editChanged);

    const bool fogBlocked = detachedEdit ? (editData.forceClearSky || editData.noFog) : (g_forceClear.load() || g_noFog.load());
    const bool windEnabled = !fogBlocked && RuntimeFeatureAvailable(RuntimeFeatureId::WindControls) && WindPackReady();
    if (!windEnabled) {
        ImGui::BeginDisabled();
    }

    DrawControlById("volume_fog_scatter_color", &editData, &overrideMask, &editChanged);

    DrawSliderById("fog_from_wind", &editData, &overrideMask, &editChanged);

    DrawControlById("mie_scatter_color", &editData, &overrideMask, &editChanged);

    DrawSliderById("mie_scale_height", &editData, &overrideMask, &editChanged);
    DrawSliderById("mie_aerosol_density", &editData, &overrideMask, &editChanged);
    DrawSliderById("mie_aerosol_absorption", &editData, &overrideMask, &editChanged);
    DrawSliderById("height_fog_baseline", &editData, &overrideMask, &editChanged);
    DrawSliderById("height_fog_falloff", &editData, &overrideMask, &editChanged);

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

    if (detachedEdit && editChanged) {
        if (regionScoped) {
            Preset_SetEditRegionDataWithOverrides(editData, overrideMask);
        } else {
            Preset_SetEditRegionData(editData);
        }
    }
}

} // namespace overlay_internal
