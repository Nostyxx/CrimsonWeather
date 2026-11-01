#include "pch.h"

#include "overlay_internal.h"
#include "performance_benchmark.h"

#include <imgui.h>

namespace overlay_internal {

static void DrawDevFieldFloat(const DevFieldSpec& spec, DevFieldState& state) {
    const uint32_t nativeBits = state.native.load();
    float native = 0.0f;
    memcpy(&native, &nativeBits, sizeof(float));
    const bool active = state.active.load();
    uint32_t bits = active ? state.value.load() : nativeBits;
    float value = 0.0f;
    memcpy(&value, &bits, sizeof(float));

    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::SliderFloat("##value", &value, spec.lo, spec.hi, "%.3f") && std::isfinite(value)) {
        memcpy(&bits, &value, sizeof(float));
        state.value.store(bits);
        state.active.store(true);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("native %.3f", native);
}

static void DrawDevFieldColor(DevFieldState& state) {
    const uint32_t native = state.native.load();
    const bool active = state.active.load();
    const uint32_t packed = active ? state.value.load() : native;
    float rgb[3] = {
        ((packed >> 16) & 0xFF) / 255.0f,
        ((packed >> 8) & 0xFF) / 255.0f,
        (packed & 0xFF) / 255.0f,
    };
    if (ImGui::ColorEdit3("##value", rgb, ImGuiColorEditFlags_NoInputs)) {
        const auto toByte = [](float v) -> uint32_t {
            return static_cast<uint32_t>(min(1.0f, max(0.0f, v)) * 255.0f + 0.5f);
        };
        // The alpha byte stays the game's.
        state.value.store((packed & 0xFF000000u) | (toByte(rgb[0]) << 16) | (toByte(rgb[1]) << 8) | toByte(rgb[2]));
        state.active.store(true);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("native %08X", native);
}

static void DrawDevFieldTests() {
    if (!ImGui::CollapsingHeader("Field tests (draft names)", ImGuiTreeNodeFlags_DefaultOpen)) {
        return;
    }
    if (!DevFieldsAvailable()) {
        ImGui::TextDisabled("Needs the DLC weather layout");
        return;
    }
    ImGui::TextWrapped("Each change logs a [dev-field-write] line. Drag to override, Reset returns to the game's value.");
    if (ImGui::Button("Reset All")) {
        for (size_t i = 0; i < DevFieldCount(); ++i) {
            DevFieldStateAt(i).active.store(false);
        }
    }

    const char* group = nullptr;
    for (size_t i = 0; i < DevFieldCount(); ++i) {
        const DevFieldSpec& spec = DevFieldSpecAt(i);
        DevFieldState& state = DevFieldStateAt(i);
        if (!group || strcmp(group, spec.group) != 0) {
            group = spec.group;
            ImGui::SeparatorText(group);
        }
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextUnformatted(spec.label);
        if (!state.nativeValid.load()) {
            ImGui::SameLine();
            ImGui::TextDisabled("(waiting for the game)");
        } else {
            if (spec.kind == DevFieldKind::Color) {
                DrawDevFieldColor(state);
            } else {
                DrawDevFieldFloat(spec, state);
            }
            if (state.active.load()) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Reset")) {
                    state.active.store(false);
                }
            }
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
}

void DrawDevTab() {
    DrawDevFieldTests();

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
