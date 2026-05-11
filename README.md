# Crimson Weather

ReShade `.addon64` weather-control mod for `CrimsonDesert.exe`.

The current main branch is the ReShade addon rewrite. The older DXGI/ImGui build is kept on the `legacy` branch.

## Install

Copy the addon into the game `bin64` folder used by ReShade:

- `CrimsonWeather.addon64`

ReShade is required for the in-game overlay. Ultimate ASI Loader is no longer required.

## Runtime Files

Main build:
- `CrimsonWeather.addon64`
- `CrimsonWeather.ini`
- `CrimsonWeather.log` when logging is enabled
- Preset `.ini` files saved next to the addon

Wind-only build:
- `CrimsonWeather.WindOnly.addon64`
- `CrimsonWeather.WindOnly.ini`
- `CrimsonWeather.WindOnly.log` when logging is enabled

## Startup

Crimson Weather starts automatically by default.

Set `AutoStart=0` in `CrimsonWeather.ini` if you want to start it manually from the ReShade overlay with `Start Crimson Weather`.

## Overlay

Open the ReShade overlay and use the `Crimson Weather` addon tab.

The edit scope selector is always visible at the top of the overlay. Use it to edit global preset values or per-region overrides from any tab.

Tabs and controls:
- **Presets**: load/save presets and reset sliders
- **General**: rain, dust, snow, force clear sky, visual time override, wind, no wind
- **Atmosphere**: cloud amount, cloud height, cloud density, mid clouds, high clouds, fog, no fog
- **Celestial**: night sky tilt, night sky phase, sun size, sun yaw/pitch lock, moon size, moon yaw/pitch lock, moon rotation
- **Experiment**: 2C, 2D, cloud variation, legacy fog, puddle scale
- **Status**: current effective values and whether each value comes from global preset or a region override

## Presets

Presets support global values plus optional per-region overrides.

Supported regions:
- Hernand
- Demeniss
- Delesyia
- Pailune
- Crimson Desert
- Abyss

Old preset files still load. Saving a preset rewrites it into the current format.

## Input

Default effect toggle hotkeys:
- Keyboard: `F10`
- Controller: `D-pad Down + A`

## Config

`CrimsonWeather.ini`

```ini
[General]
LogEnabled=0
AutoStart=1
HotkeyToggleEffect=F10
_HotkeyOptions=F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, or single letter A-Z

[Hotkeys]
ControllerToggleEffect=dpad_down+a
_ControllerHotkeyOptions=Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back

[Preset]
LastPreset=
```

## Build

Open:
- `CrimsonWeather.slnx`

Build both release addons:
- `.\build_release_dual.ps1`

Compiler output:
- `CrimsonWeatherReshade/x64/Release/CrimsonWeather.addon64`
- `CrimsonWeatherReshade/x64/Release/CrimsonWeather.WindOnly.addon64`

## Download

- Nexus Mods: https://www.nexusmods.com/crimsondesert/mods/632
