# CrimsonWeather

ReShade add-on weather-control mod for `CrimsonDesert.exe`.

Current main branch is the ReShade add-on rewrite. The older DXGI/ImGui build is kept on the `legacy` branch.

## Runtime Files

- `CrimsonWeather.addon64`
- `CrimsonWeather.ini`
- `CrimsonWeather.log` when logging is enabled
- `*.ini` preset files saved in the same mod directory

## Overlay

This version uses a ReShade-hosted overlay instead of the older custom DXGI overlay.
Open the Crimson Weather overlay and press `Start Crimson Weather` once per game launch.

Current controls:
- Presets
- Rain
- Dust
- Snow
- Force Clear Sky
- Visual Time Override
- Cloud height / density / mid clouds / high clouds
- Fog
- Wind / No Wind
- 2C / 2D / Night Sky Rotation
- Puddle Scale

## Input

Default effect toggle hotkeys:
- Keyboard: `F10`
- Controller: `D-pad Down + A`

## Config

`CrimsonWeather.ini`

```ini
[General]
LogEnabled=0
HotkeyToggleEffect=F10
_HotkeyOptions=F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, or single letter A-Z

[Hotkeys]
ControllerToggleEffect=dpad_down+a
_ControllerHotkeyOptions=Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back

[Preset]
LastPreset=
```

Notes:
- ReShade is required for the in-game UI.
- A ReShade build with add-on support is required.
- No ASI loader is required.
- Old preset files still load.
- Saving a preset rewrites it into the current format.

## Build

Open:
- `CrimsonWeather.slnx`

Build:
- `Release | x64`

Compiler output:
- `CrimsonWeatherReshade/x64/Release/CrimsonWeather.addon64`

## Download

- Nexus Mods: https://www.nexusmods.com/crimsondesert/mods/632
