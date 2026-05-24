#include "pch.h"

#include "overlay_internal.h"
#include "renodx_bridge.h"

#include <imgui.h>

#include <iterator>

namespace overlay_internal {
namespace {

struct RenoDxRegionToggle {
    uint32_t bit;
    int regionId;
};

constexpr RenoDxRegionToggle kAuroraRegionToggles[] = {
    { kRenoDxAuroraRegionHernand, kPresetRegionHernand },
    { kRenoDxAuroraRegionDemeniss, kPresetRegionDemeniss },
    { kRenoDxAuroraRegionDelesyia, kPresetRegionDelesyia },
    { kRenoDxAuroraRegionPailune, kPresetRegionPailune },
    { kRenoDxAuroraRegionCrimsonDesert, kPresetRegionCrimsonDesert },
    { kRenoDxAuroraRegionAbyss, kPresetRegionAbyss },
};

} // namespace

void DrawRenoDxInteractionControls() {
    ImGui::Spacing();
    ImGui::SeparatorText("RenoDX Interaction");

    const bool detected = RenoDxBridgeIsAddonPresent();
    if (!detected) {
        ImGui::BeginDisabled();
    }

    uint32_t mask = RenoDxBridgeGetAuroraRegionMask();
    uint32_t newMask = mask;
    bool auroraEnabled = RenoDxBridgeIsAuroraGateEnabled();
    bool newAuroraEnabled = auroraEnabled;

    ImGui::Checkbox("Allowed Aurora Regions", &newAuroraEnabled);
    if (!newAuroraEnabled) {
        ImGui::BeginDisabled();
    }
    for (size_t i = 0; i < std::size(kAuroraRegionToggles); ++i) {
        const auto& toggle = kAuroraRegionToggles[i];
        bool selected = (newMask & toggle.bit) != 0;
        if (ImGui::RadioButton(Preset_GetRegionDisplayName(toggle.regionId), selected)) {
            selected = !selected;
            if (selected) {
                newMask |= toggle.bit;
            } else {
                newMask &= ~toggle.bit;
            }
        }
    }
    if (!newAuroraEnabled) {
        ImGui::EndDisabled();
    }

    if (!detected) {
        ImGui::EndDisabled();
        ImGui::TextDisabled("renodx-crimsondesert.addon64 not detected");
    }

    if (newAuroraEnabled != auroraEnabled) {
        RenoDxBridgeSetAuroraGateEnabled(newAuroraEnabled);
        Preset_SetRenoDxAuroraSettings(newAuroraEnabled, newMask);
        GUI_SetStatus(newAuroraEnabled ? "RenoDX aurora gate enabled" : "RenoDX aurora gate disabled");
    }
    if (newMask != mask) {
        RenoDxBridgeSetAuroraRegionMask(newMask);
        Preset_SetRenoDxAuroraSettings(newAuroraEnabled, newMask);
        GUI_SetStatus("RenoDX aurora regions changed");
    }
}

} // namespace overlay_internal
