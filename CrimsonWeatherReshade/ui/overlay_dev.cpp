#include "pch.h"

#include "overlay_internal.h"
#include "performance_benchmark.h"

#include <imgui.h>

namespace overlay_internal {

void DrawDevTab() {
    if (ImGui::CollapsingHeader("Automated Performance Diagnostic", ImGuiTreeNodeFlags_DefaultOpen)) {
        const PerformanceBenchmarkStatus status = PerformanceBenchmarkGetStatus();
        if (!status.active) {
            if (ImGui::Button("Start Full Diagnostic", ImVec2(190.0f, 0.0f))) {
                char error[192] = {};
                if (!PerformanceBenchmarkStart(error, sizeof(error))) {
                    GUI_SetStatus(error[0] ? error : "Performance diagnostic failed to start");
                } else {
                    GUI_SetStatus("Performance diagnostic started; close the overlay");
                }
            }
        } else if (ImGui::Button("Abort And Restore", ImVec2(190.0f, 0.0f))) {
            PerformanceBenchmarkAbort();
        }

        ImGui::SameLine();
        ImGui::Text("%s", status.phase);
        if (status.scenarioCount > 0) {
            ImGui::Text("Scenario: %zu / %zu  %s", status.scenarioIndex, status.scenarioCount, status.scenario);
            ImGui::ProgressBar(status.overallProgress, ImVec2(-1.0f, 0.0f));
            const unsigned int minutes = status.estimatedSecondsRemaining / 60;
            const unsigned int seconds = status.estimatedSecondsRemaining % 60;
            ImGui::TextDisabled("Estimated remaining: %u:%02u", minutes, seconds);
        }
        ImGui::TextWrapped("%s", status.message);
        if (status.reportPath && status.reportPath[0]) {
            ImGui::TextWrapped("Report: %s", status.reportPath);
        }
        ImGui::Spacing();
    }
}

} // namespace overlay_internal
