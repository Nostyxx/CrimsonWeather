#include "pch.h"

#include "overlay_internal.h"
#include "preset_service.h"
#include "runtime_shared.h"

#include <imgui.h>

namespace overlay_internal {

void DrawWaterTab() {
    if (DrawDisabledTabBody()) {
        return;
    }

    if (!g_waterTableReady.load()) {
        ImGui::TextDisabled("Water controls are not available on this game version.");
        return;
    }

    const bool detachedEdit = Preset_IsEditingDetachedRegion();
    WeatherPresetData editData = detachedEdit ? Preset_GetEditRegionData() : WeatherPresetData{};
    const bool regionScoped = detachedEdit && Preset_GetEditRegion() > kPresetRegionGlobal;
    WeatherPresetSourceMask overrideMask = regionScoped ? Preset_GetEditRegionOverrideMask() : WeatherPresetSourceMask{};
    bool editChanged = false;

    ImGui::SeparatorText("Water");
    DrawSliderById("water_murkiness", &editData, &overrideMask, &editChanged);
    DrawControlById("murk_color", &editData, &overrideMask, &editChanged);
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Colours the murk in the water. Needs Murkiness above 0.\nStrongest on the sea. In rivers it mainly shows underwater.");
    }

    ImGui::SeparatorText("Waves");
    DrawSliderById("water_ripples", &editData, &overrideMask, &editChanged);
    DrawSliderById("water_chop", &editData, &overrideMask, &editChanged);
    DrawSliderById("water_waves", &editData, &overrideMask, &editChanged);
    DrawSliderById("water_swell", &editData, &overrideMask, &editChanged);

    ImGui::SeparatorText("Underwater");
    DrawControlById("underwater_tint_color", &editData, &overrideMask, &editChanged);
    DrawSliderById("underwater_tint_strength", &editData, &overrideMask, &editChanged);
    DrawSliderById("underwater_fog", &editData, &overrideMask, &editChanged);

    if (detachedEdit && editChanged) {
        if (regionScoped) {
            Preset_SetEditRegionDataWithOverrides(editData, overrideMask);
        } else {
            Preset_SetEditRegionData(editData);
        }
    }
}

} // namespace overlay_internal
