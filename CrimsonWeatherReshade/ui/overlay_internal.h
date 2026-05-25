#pragma once

#include "preset_service.h"
#include "runtime_shared.h"

#include <cstddef>

namespace overlay_internal {

struct SliderRange {
    float lo;
    float hi;
};

const char* NumericInputFormat(const char* format, char* out, size_t outSize);
int HourToMinuteOfDay(float hour);
int HourToMinuteOfDayFloor(float hour);
float MinuteOfDayToHour(int minuteOfDay);
void FormatGameClockFromMinute(int minuteOfDay, char* out, size_t outSize);
void FormatGameClockFromHour(float hour, char* out, size_t outSize);
bool TryParseClockText(const char* text, int* outMinuteOfDay);
bool TryGetHudGameTimeHour(float* outHour);
float ClampProgressVisualTimeIntervalMs(float value);
void FormatProgressVisualTimeInterval(float value, char* out, size_t outSize);

SliderRange ActiveSliderRange(float normalLo, float normalHi, float extendedLo, float extendedHi);
float ClampSliderValue(float value, const SliderRange& range);
void ClampColorValues(float* color, bool includeAlpha);
WeatherPresetColor ColorFromArray(const float* color, bool includeAlpha);
WeatherPresetColor RayleighColorFromUiBits(unsigned int bits);

void DrawFeatureUnavailable(RuntimeFeatureId feature);
void DrawHookUnavailable(RuntimeHookId hook);
bool WeatherTickReady();
bool RainHookReady();
bool SnowHookReady();
bool DustHookReady();
bool WindPackReady();
bool SceneFrameReady();
bool WeatherFrameReady();
bool RealGameTimeReady();
bool DrawResetButton(const char* id);
bool DrawOverrideToggle(bool* enabled);
void DrawOverrideBadge(bool regionOverride);
void DrawTexturePackHeader(const char* packName);
bool DrawSliderFloatRow(
    const char* label,
    const char* id,
    float* value,
    float minValue,
    float maxValue,
    const char* format,
    bool* outValueChanged,
    bool* overrideEnabled,
    bool* outOverrideChanged,
    bool nativeDisplay,
    bool centerOnNative = false,
    float nativeValue = 1.0f);
bool DrawColorRow(
    const char* label,
    const char* id,
    float* color,
    bool includeAlpha,
    bool* outValueChanged,
    bool* overrideEnabled,
    bool* outOverrideChanged,
    bool nativeDisplay);
bool DrawOverrideCheckboxRow(
    const char* label,
    const char* id,
    bool* value,
    bool* overrideEnabled,
    bool* outOverrideChanged);
bool DrawClockDial(const char* id, int* minuteOfDay, bool centerDial = true);
bool TextContainsNoCase(const char* text, const char* needle);
bool DrawDisabledTabBody();
void DrawSliderById(
    const char* id,
    WeatherPresetData* editData = nullptr,
    WeatherPresetSourceMask* overrideMask = nullptr,
    bool* editChanged = nullptr);
void DrawControlById(
    const char* id,
    WeatherPresetData* editData = nullptr,
    WeatherPresetSourceMask* overrideMask = nullptr,
    bool* editChanged = nullptr);
void DrawTimeControls();
void DrawGeneralControls();
void DrawRenoDxInteractionControls();

bool IsSliderTextEditActive(const char* id);
bool ConsumeSliderTextEditFocusRequest();
void BeginSliderTextEdit(const char* id);
void EndSliderTextEdit();

void DrawStatusTab();
void DrawPresetTab();
void DrawFavoritesTab();
void DrawWeatherTab();
void DrawGeneralTab();
void DrawAtmosphereTab();
void DrawExperimentTab();
void DrawCelestialTab();
#if defined(CW_DEV_BUILD)
void DrawDevTab();
#endif

} // namespace overlay_internal
