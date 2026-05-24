#include "pch.h"

#include "overlay_bridge.h"
#include "community_ui.h"
#include "sky_texture_override.h"
#include "preset_service.h"
#include "runtime_shared.h"
#include "update_service.h"
#include "overlay_internal.h"

#include <imgui.h>
#include <reshade.hpp>

#include <d3d12.h>
#include <cmath>
#include <cstdio>
#include <string>

using namespace overlay_internal;

namespace {

HMODULE g_overlayModule = nullptr;
bool g_overlayRegistered = false;
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

void DrawFooterLinksRight(float rightEdge);

void DrawWindOnlyOverlay() {
    ImGui::Text("%s %s", MOD_DISPLAY_NAME, MOD_VERSION);
    const float cursorY = ImGui::GetCursorPosY() - ImGui::GetTextLineHeightWithSpacing();
    const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    ImGui::SameLine();
    ImGui::SetCursorPosY(cursorY);
    DrawFooterLinksRight(rightEdge);
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


std::string BuildFooterScheduleLabel() {
    const PresetScheduleStatus status = PresetSchedule_GetStatus();
    if (!status.enabled) {
        return "OFF";
    }
    if (status.blending && !status.blendToDisplayName.empty()) {
        return status.blendToDisplayName;
    }
    if (status.active && !status.activeDisplayName.empty()) {
        return status.activeDisplayName;
    }
    return "ON";
}

void DrawFooterLinksRight(float rightEdge) {
    const char* kofiLabel = "kofi";
    const char* nexusLabel = "nexusmods";
    const char* separator = "|";
    const float spacing = ImGui::GetStyle().ItemSpacing.x;
    const float linksWidth = ImGui::CalcTextSize(kofiLabel).x
        + spacing + ImGui::CalcTextSize(separator).x
        + spacing + ImGui::CalcTextSize(nexusLabel).x;

    ImGui::SetCursorPosX(max(ImGui::GetCursorPosX(), rightEdge - linksWidth));
    ImGui::TextLinkOpenURL(kofiLabel, "https://ko-fi.com/nostyx");
    ImGui::SameLine();
    ImGui::TextUnformatted(separator);
    ImGui::SameLine();
    ImGui::TextLinkOpenURL(nexusLabel, "https://www.nexusmods.com/crimsondesert/mods/632");
}

void DrawUpdateChangelogPopup(const UpdateCheckInfo& updateInfo) {
    if (!ImGui::BeginPopupModal("Crimson Weather Changelog", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const std::string title = updateInfo.title.empty()
        ? ("Update " + updateInfo.latestVersion)
        : updateInfo.title;
    ImGui::TextUnformatted(title.c_str());
    ImGui::Separator();

    const ImVec2 childSize(620.0f, min(420.0f, ImGui::GetTextLineHeightWithSpacing() * 24.0f));
    if (ImGui::BeginChild("UpdateChangelogText", childSize, true)) {
        if (!updateInfo.changelog.empty()) {
            ImGui::TextUnformatted(updateInfo.changelog.c_str());
        } else {
            ImGui::TextDisabled("No changelog was provided for this update.");
        }
    }
    ImGui::EndChild();

    if (updateInfo.state != UpdateCheckState::Installed) {
        const bool directInstall = g_cfg.updaterAutoDownload;
        const bool installReady = directInstall && !updateInfo.addonDownloadUrl.empty() && !updateInfo.addonSha256.empty();
        if (updateInfo.downloading || (directInstall && !installReady)) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(directInstall ? "Install Update" : "Download", ImVec2(140.0f, 0.0f))) {
            if (directInstall) {
                UpdateService_InstallUpdate();
            } else {
                UpdateService_OpenDownloadPage();
            }
        }
        if (updateInfo.downloading || (directInstall && !installReady)) {
            ImGui::EndDisabled();
        }
        if (directInstall && !installReady && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Direct update package is not available yet. Disable AutoDownload to open the Nexus page instead.");
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Close", ImVec2(120.0f, 0.0f))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawUpdateHeader() {
    UpdateService_Tick();
    const UpdateCheckInfo updateInfo = UpdateService_GetInfo();

    ImGui::Text("%s %s", MOD_NAME, MOD_VERSION);
    ImGui::SameLine();

    switch (updateInfo.state) {
    case UpdateCheckState::Latest:
        ImGui::TextDisabled("| (Latest)");
        break;
    case UpdateCheckState::Installed:
        ImGui::TextColored(ImVec4(0.40f, 0.88f, 0.48f, 1.0f), "| (UPDATE SUCCESS. RESTART TO APPLY)");
        break;
    case UpdateCheckState::Downloading:
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.28f, 1.0f), "| (DOWNLOADING UPDATE...)");
        break;
    case UpdateCheckState::UpdateAvailable: {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.28f, 1.0f), "| (UPDATE AVAILABLE)");
        ImGui::SameLine();
        if (ImGui::Button("Changelog")) {
            ImGui::OpenPopup("Crimson Weather Changelog");
        }
        ImGui::SameLine();
        const bool directInstall = g_cfg.updaterAutoDownload;
        const bool installReady = directInstall && !updateInfo.addonDownloadUrl.empty() && !updateInfo.addonSha256.empty();
        if (directInstall && !installReady) {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button(directInstall ? "Install Update" : "Download")) {
            if (directInstall) {
                UpdateService_InstallUpdate();
            } else {
                UpdateService_OpenDownloadPage();
            }
        }
        if (directInstall && !installReady) {
            ImGui::EndDisabled();
        }
        if (directInstall && !installReady && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Direct update package is not available yet. Disable AutoDownload to open the Nexus page instead.");
        }
        break;
    }
    case UpdateCheckState::Checking:
        ImGui::TextDisabled("| (Checking...)");
        break;
    case UpdateCheckState::Error:
        ImGui::TextDisabled("| (Update check failed)");
        break;
    case UpdateCheckState::Disabled:
        ImGui::TextDisabled("| (Update check disabled)");
        break;
    case UpdateCheckState::Idle:
    default:
        ImGui::TextDisabled("| (Checking soon)");
        break;
    }

    DrawUpdateChangelogPopup(updateInfo);
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

    DrawUpdateHeader();
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
        if (ImGui::BeginTabItem("Community")) {
            DrawCommunityTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Favorites")) {
            DrawFavoritesTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("General")) {
            DrawGeneralTab();
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
    const std::string scheduleLabel = BuildFooterScheduleLabel();
    ImGui::Text("UNSAVED : %s  |  SCHEDULE : %s  |  EDITING : %s",
        Preset_HasUnsavedChanges() ? "YES" : "NO",
        scheduleLabel.c_str(),
        Preset_GetSelectedDisplayName());
    Preset_AutoSaveTick(ImGui::IsAnyItemActive());
    const float rightEdge = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    ImGui::SameLine();
    DrawFooterLinksRight(rightEdge);
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

static void OnReShadePresent(reshade::api::command_queue*,
                             reshade::api::swapchain*,
                             const reshade::api::rect*,
                             const reshade::api::rect*,
                             uint32_t,
                             const reshade::api::rect*) {
    SkyTextureOnPresent();
}

} // namespace

bool InitializeOverlayBridge(HMODULE module) {
    if (!reshade::register_addon(module)) {
        return false;
    }

    reshade::register_overlay(MOD_NAME, &DrawOverlay);

    reshade::register_event<reshade::addon_event::init_device>(&OnReShadeInitDevice);
    reshade::register_event<reshade::addon_event::present>(&OnReShadePresent);

    g_overlayModule = module;
    g_overlayRegistered = true;
    return true;
}

void ShutdownOverlayBridge() {
    if (!g_overlayRegistered) {
        return;
    }

    reshade::unregister_event<reshade::addon_event::present>(&OnReShadePresent);
    reshade::unregister_event<reshade::addon_event::init_device>(&OnReShadeInitDevice);

    reshade::unregister_overlay(MOD_NAME, &DrawOverlay);
    reshade::unregister_addon(g_overlayModule);
    g_overlayRegistered = false;
    g_overlayModule = nullptr;
}
