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
char g_moonTextureFilter[96] = "";
char g_milkywayTextureFilter[96] = "";
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
                const bool optionAnimated = i > 0 && MilkywayTextureOptionIsAnimated(i);
                const bool visible = i == 0
                    ? TextContainsNoCase("Native", g_milkywayTextureFilter)
                    : (TextContainsNoCase(optionLabel, g_milkywayTextureFilter) ||
                       TextContainsNoCase(optionName, g_milkywayTextureFilter) ||
                       TextContainsNoCase(MilkywayTextureOptionPack(i), g_milkywayTextureFilter) ||
                       (optionAnimated && TextContainsNoCase("anim", g_milkywayTextureFilter)));
                if (!visible) {
                    continue;
                }
                const char* optionPack = i > 0 ? MilkywayTextureOptionPack(i) : "";
                const bool optionHasPack = optionPack && optionPack[0];
                if (optionHasPack) {
                    if (!currentPack || strcmp(currentPack, optionPack) != 0) {
                        DrawTexturePackHeader(optionPack);
                        currentPack = optionPack;
                    }
                }
                ++visibleCount;
                const bool selected = i == milkywayTexture;
                if (optionHasPack) {
                    ImGui::Indent(12.0f);
                }
                char optionDisplay[256] = {};
                sprintf_s(optionDisplay, sizeof(optionDisplay), "%s%s", optionLabel, optionAnimated ? " [ANIM]" : "");
                if (ImGui::Selectable(optionDisplay, selected)) {
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
                if (optionHasPack) {
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

    DrawSliderById("night_sky_tilt", &editData, &overrideMask, &editChanged);
    if (!celestialWindPackReady) {
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::WindPack);
        }
    }

    DrawSliderById("night_sky_phase", &editData, &overrideMask, &editChanged);
    if (!celestialSceneFrameReady) {
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::SceneFrameUpdate);
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Sun");

    DrawSliderById("sun_light_intensity", &editData, &overrideMask, &editChanged);
    if (!celestialWindPackReady) {
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::WindPack);
        }
    }

    DrawSliderById("sun_size", &editData, &overrideMask, &editChanged);
    if (!celestialWindPackReady) {
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::WindPack);
        }
    }

    DrawSliderById("sun_yaw", &editData, &overrideMask, &editChanged);
    if (!celestialSceneFrameReady) {
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::SceneFrameUpdate);
        }
    }

    DrawSliderById("sun_pitch", &editData, &overrideMask, &editChanged);
    if (!celestialSceneFrameReady) {
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
                const bool optionAnimated = i > 0 && MoonTextureOptionIsAnimated(i);
                const bool visible = i == 0
                    ? TextContainsNoCase("Native", g_moonTextureFilter)
                    : (TextContainsNoCase(optionLabel, g_moonTextureFilter) ||
                       TextContainsNoCase(optionName, g_moonTextureFilter) ||
                       TextContainsNoCase(MoonTextureOptionPack(i), g_moonTextureFilter) ||
                       (optionAnimated && TextContainsNoCase("anim", g_moonTextureFilter)));
                if (!visible) {
                    continue;
                }
                const char* optionPack = i > 0 ? MoonTextureOptionPack(i) : "";
                const bool optionHasPack = optionPack && optionPack[0];
                if (optionHasPack) {
                    if (!currentPack || strcmp(currentPack, optionPack) != 0) {
                        DrawTexturePackHeader(optionPack);
                        currentPack = optionPack;
                    }
                }
                ++visibleCount;
                const bool selected = i == moonTexture;
                if (optionHasPack) {
                    ImGui::Indent(12.0f);
                }
                char optionDisplay[256] = {};
                sprintf_s(optionDisplay, sizeof(optionDisplay), "%s%s", optionLabel, optionAnimated ? " [ANIM]" : "");
                if (ImGui::Selectable(optionDisplay, selected)) {
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
                if (optionHasPack) {
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

    DrawSliderById("moon_light_intensity", &editData, &overrideMask, &editChanged);
    if (!celestialWindPackReady) {
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::WindPack);
        }
    }

    DrawSliderById("moon_size", &editData, &overrideMask, &editChanged);
    if (!celestialWindPackReady) {
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::WindPack);
        }
    }

    DrawSliderById("moon_yaw", &editData, &overrideMask, &editChanged);
    if (!celestialSceneFrameReady) {
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::SceneFrameUpdate);
        }
    }

    DrawSliderById("moon_pitch", &editData, &overrideMask, &editChanged);
    if (!celestialSceneFrameReady) {
        if (celestialEnabled) {
            DrawHookUnavailable(RuntimeHookId::SceneFrameUpdate);
        }
    }

    DrawSliderById("moon_roll", &editData, &overrideMask, &editChanged);
    if (!celestialSceneFrameReady) {
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

} // namespace overlay_internal
