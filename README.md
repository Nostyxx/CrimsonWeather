# CrimsonWeather

ASI weather-control mod for `CrimsonDesert.exe`.

Current stable branch focuses on practical runtime weather controls through an in-game DX12 overlay:
- Rain
- Dust
- Snow
- Visual Time Override
- Cloud height / density / mid clouds / high clouds
- Fog
- Wind / No Wind
- Detail controls
- Preset save/load


## Runtime Files

- `CrimsonWeather.asi`
- `CrimsonWeather.ini`
- `CrimsonWeather.log` when logging is enabled
- `*.ini` preset files saved in the same mod directory

## Overlay / Input

The overlay uses ImGui on DX12 and supports:
- keyboard hotkeys
- XInput pad navigation
- GameInput fallback for non-XInput controllers
- native DualSense raw-input path when a supported Sony HID device is present

Default hotkeys:
- Toggle GUI: `F9`
- Toggle weather control: `F10`
- Controller GUI toggle: `D-pad Down + B`
- Controller effect toggle: `D-pad Down + X`

## Config

`CrimsonWeather.ini`

```ini
[General]
LogEnabled=1
HotkeyToggleGUI=F9
HotkeyToggleEffect=F10

[Hotkeys]
ControllerHotkeyToggleGUI=dpad_down+b
ControllerToggleEffect=dpad_down+x

[UI]
Scale=1.00
ShowOnStartup=1
```

Notes:
- `LogEnabled=1` writes startup and runtime diagnostics to `CrimsonWeather.log`.
- `Scale` is clamped to the supported UI range.
- `ShowOnStartup=1` opens the overlay automatically on startup.
- Preset selection is remembered in the INI and auto-applied when the world is ready.

## Build

Open:
- `CrimsonWeatherMod/CrimsonWeatherMod.sln`

Build:
- `Release | x64`

Compiler output:
- `CrimsonWeatherMod/x64/Release/CrimsonWeather.dll`

Runtime deployment name:
- `CrimsonWeather.asi`

Current post-build deploy target:
- `C:\Program Files (x86)\Steam\steamapps\common\Crimson Desert\CrimsonWeather.asi`
