#pragma once

#include <cstdint>
#include <cstddef>

#include <imgui.h>
#include <reshade.hpp>

constexpr uint32_t kRenoDxAuroraRegionHernand = 1u << 1;
constexpr uint32_t kRenoDxAuroraRegionDemeniss = 1u << 2;
constexpr uint32_t kRenoDxAuroraRegionDelesyia = 1u << 3;
constexpr uint32_t kRenoDxAuroraRegionPailune = 1u << 4;
constexpr uint32_t kRenoDxAuroraRegionCrimsonDesert = 1u << 5;
constexpr uint32_t kRenoDxAuroraRegionAbyss = 1u << 6;
constexpr uint32_t kRenoDxAuroraAllRegions =
    kRenoDxAuroraRegionHernand |
    kRenoDxAuroraRegionDemeniss |
    kRenoDxAuroraRegionDelesyia |
    kRenoDxAuroraRegionPailune |
    kRenoDxAuroraRegionCrimsonDesert |
    kRenoDxAuroraRegionAbyss;

bool RenoDxBridgeIsAddonPresent();
bool RenoDxBridgeIsAuroraGateEnabled();
void RenoDxBridgeSetAuroraGateEnabled(bool enabled);
uint32_t RenoDxBridgeGetAuroraRegionMask();
void RenoDxBridgeSetAuroraRegionMask(uint32_t mask);
void RenoDxBridgeApplyPresetAuroraSettings(bool enabled, uint32_t mask);
bool RenoDxBridgeIsCurrentRegionAllowed();

void RenoDxBridgeOnPresent();
void RenoDxBridgeOnBeginEffects(
    reshade::api::effect_runtime* runtime,
    reshade::api::command_list* cmdList,
    reshade::api::resource_view rtv,
    reshade::api::resource_view rtvSrgb);
bool RenoDxBridgeOnSetUniformValue(
    reshade::api::effect_runtime* runtime,
    reshade::api::effect_uniform_variable variable,
    const void* data,
    size_t size);
