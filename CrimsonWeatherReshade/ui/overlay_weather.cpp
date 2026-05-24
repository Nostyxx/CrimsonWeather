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
void DrawWeatherTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData editData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask overrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    bool editChanged = false;

    ImGui::SeparatorText("Weather");
    DrawControlById("force_clear", &editData, &overrideMask, &editChanged);
    DrawControlById("no_rain", &editData, &overrideMask, &editChanged);
    DrawControlById("no_dust", &editData, &overrideMask, &editChanged);
    DrawControlById("no_snow", &editData, &overrideMask, &editChanged);
    const bool forceClear = detachedEdit ? editData.forceClearSky : g_forceClear.load();
    const bool noRain = detachedEdit ? editData.noRain : g_noRain.load();
    const bool noDust = detachedEdit ? editData.noDust : g_noDust.load();
    const bool noSnow = detachedEdit ? editData.noSnow : g_noSnow.load();

    ImGui::Spacing();
    ImGui::SeparatorText("Rain");
    const bool rainEnabled = !forceClear && !noRain && RuntimeFeatureAvailable(RuntimeFeatureId::Rain) && RainHookReady();
    DrawSliderById("rain", &editData, &overrideMask, &editChanged);
    if (!rainEnabled) {
        if (!forceClear && !noRain) {
            if (!RuntimeFeatureAvailable(RuntimeFeatureId::Rain)) {
                DrawFeatureUnavailable(RuntimeFeatureId::Rain);
            } else {
                DrawHookUnavailable(RuntimeHookId::GetRainIntensity);
            }
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Dust");
    const bool dustEnabled = !forceClear && !noDust && RuntimeFeatureAvailable(RuntimeFeatureId::Dust) && DustHookReady();
    DrawSliderById("dust", &editData, &overrideMask, &editChanged);
    if (!dustEnabled) {
        if (!forceClear && !noDust) {
            if (!RuntimeFeatureAvailable(RuntimeFeatureId::Dust)) {
                DrawFeatureUnavailable(RuntimeFeatureId::Dust);
            } else {
                DrawHookUnavailable(RuntimeHookId::GetDustIntensity);
            }
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Snow");
    const bool snowEnabled = !forceClear && !noSnow && RuntimeFeatureAvailable(RuntimeFeatureId::Snow) && SnowHookReady();
    DrawSliderById("snow", &editData, &overrideMask, &editChanged);
    if (!snowEnabled) {
        if (!forceClear && !noSnow) {
            if (!RuntimeFeatureAvailable(RuntimeFeatureId::Snow)) {
                DrawFeatureUnavailable(RuntimeFeatureId::Snow);
            } else {
                DrawHookUnavailable(RuntimeHookId::GetSnowIntensity);
            }
        }
    }

    const bool advancedSnowEnabled = !forceClear && !noSnow && WeatherTickReady();
    DrawSliderById("snow_accum_boundary_a", &editData, &overrideMask, &editChanged);
    DrawSliderById("snow_accum_boundary_b", &editData, &overrideMask, &editChanged);
    DrawSliderById("snow_coverage_threshold", &editData, &overrideMask, &editChanged);
    if (!advancedSnowEnabled && !forceClear && !noSnow) {
        DrawHookUnavailable(RuntimeHookId::WeatherTick);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Thunder");
    const bool thunderEnabled = !forceClear && RuntimeFeatureAvailable(RuntimeFeatureId::ThunderControls) && WeatherTickReady();
    DrawSliderById("thunder", &editData, &overrideMask, &editChanged);
    if (!thunderEnabled) {
        if (!RuntimeFeatureAvailable(RuntimeFeatureId::ThunderControls)) {
            DrawFeatureUnavailable(RuntimeFeatureId::ThunderControls);
        } else if (!forceClear) {
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
