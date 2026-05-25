#include "pch.h"

#include "overlay_internal.h"

#include <imgui.h>

namespace overlay_internal {

void DrawDevTab() {
    if (ImGui::CollapsingHeader("Real Game Time Getter", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool enabled = g_devGameTimeOverrideEnabled.load();
        if (ImGui::Checkbox("Override GameTimeGetter Output", &enabled)) {
            g_devGameTimeOverrideEnabled.store(enabled);
            g_devGameTimeResetAnchor.store(true);
            g_devGameTimeOverrideCallCount.store(0);
            GUI_SetStatus(enabled ? "DEV game time getter override enabled" : "DEV game time getter override disabled");
        }

        const char* modes[] = { "Offset", "Fixed Clock", "Scaled Clock" };
        int mode = g_devGameTimeOverrideMode.load();
        if (ImGui::Combo("Mode", &mode, modes, IM_ARRAYSIZE(modes))) {
            g_devGameTimeOverrideMode.store(mode);
            g_devGameTimeResetAnchor.store(true);
            g_devGameTimeOverrideCallCount.store(0);
        }

        int offsetMinutes = g_devGameTimeOffsetMinutes.load();
        if (ImGui::InputInt("Offset Minutes", &offsetMinutes)) {
            g_devGameTimeOffsetMinutes.store(std::clamp(offsetMinutes, -10080, 10080));
        }

        int fixedHour = g_devGameTimeFixedHour.load();
        int fixedMinute = g_devGameTimeFixedMinute.load();
        bool fixedChanged = false;
        fixedChanged |= ImGui::InputInt("Fixed Hour", &fixedHour);
        fixedChanged |= ImGui::InputInt("Fixed Minute", &fixedMinute);
        if (fixedChanged) {
            g_devGameTimeFixedHour.store(std::clamp(fixedHour, 0, 23));
            g_devGameTimeFixedMinute.store(std::clamp(fixedMinute, 0, 59));
        }

        float scale = g_devGameTimeScale.load();
        if (ImGui::SliderFloat("Scale Multiplier", &scale, 0.0f, 10.0f, "%.3f")) {
            g_devGameTimeScale.store(std::max(0.0f, scale));
        }
        int minuteMs = g_devGameTimeMinuteMs.load();
        if (ImGui::InputInt("Target Real ms / Game Minute", &minuteMs)) {
            g_devGameTimeMinuteMs.store(std::max(1, minuteMs));
            g_devGameTimeResetAnchor.store(true);
        }

        if (ImGui::Button("Reset Scaled Clock Anchor")) {
            g_devGameTimeResetAnchor.store(true);
            GUI_SetStatus("DEV game time scale anchor reset");
        }
        ImGui::SameLine();
        if (ImGui::Button("Commit Current Virtual Time")) {
            g_devGameTimeCommitOnce.store(true);
            GUI_SetStatus("DEV game time commit requested");
        }
        bool writeNative = g_devGameTimeWriteNative.load();
        if (ImGui::Checkbox("Continuously Write Virtual Time To Native Source", &writeNative)) {
            g_devGameTimeWriteNative.store(writeNative);
            g_devGameTimeWriteCount.store(0);
            GUI_SetStatus(writeNative ? "DEV native time writeback enabled" : "DEV native time writeback disabled");
        }

        if (ImGui::Button("Native Getter Output")) {
            g_devGameTimeOverrideEnabled.store(false);
            g_devGameTimeAnchorValid.store(false);
            GUI_SetStatus("DEV game time getter restored");
        }

        const int day = g_gameTimeProbeDay.load();
        const int hour = g_gameTimeProbeHour.load();
        const int minute = g_gameTimeProbeMinute.load();
        const int second = g_gameTimeProbeSecond.load();
        const int ms = g_gameTimeProbeMillisecond.load();
        ImGui::Text("Getter: day %d %02d:%02d:%02d.%03d", day, hour, minute, second, ms);
        ImGui::Text("HUD: %02d:%02d", g_timeUiClockHour24.load(), g_timeUiClockMinute.load());
        ImGui::Text("Override calls: %llu", g_devGameTimeOverrideCallCount.load());
        ImGui::Text("Native writes: %llu  Storage: 0x%llX",
            g_devGameTimeWriteCount.load(),
            g_devGameTimeLastStorage.load());
        ImGui::Text("Native ms: %lld  Virtual ms: %lld",
            g_devGameTimeLastNativeMs.load(),
            g_devGameTimeLastVirtualMs.load());
    }
}

} // namespace overlay_internal
