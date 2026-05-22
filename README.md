# Crimson Weather

ReShade `.addon64` weather-control mod for `CrimsonDesert.exe`.

Current stable release: `0.6.5`.

The current main branch is the ReShade addon rewrite. The older DXGI/ImGui build is kept on the `legacy` branch.

## Install

Install ReShade with addon support for `CrimsonDesert.exe`, then copy the addon into the game `bin64` folder used by ReShade:

- `CrimsonWeather.addon64`

ReShade is required for the in-game overlay. Ultimate ASI Loader is not required.

## Runtime Files

Main build:
- `CrimsonWeather.addon64`
- `CrimsonWeather.ini`
- `CrimsonWeather.log` when logging is enabled
- Preset `.ini` files saved next to the addon
- Downloaded community preset `.ini` files in `CrimsonWeather/community/preset`
- Community catalog cache in `CrimsonWeather/community/catalog.v1.json`
- Optional moon textures in `CrimsonWeather/moon/{Pack Name}/{Moon Name}/moon.dds` or `moon.png`
- Optional animated moon texture packs in `CrimsonWeather/moon/{Pack Name}/{Moon Name}/manifest.json`
- Optional Milky Way textures in `CrimsonWeather/milkyway/{Pack Name}/{Sky Name}/milkyway.dds` or `milkyway.png`

The `moon` and `milkyway` folders are created automatically if they are missing.

Wind-only build:
- `CrimsonWeather.WindOnly.addon64`
- `CrimsonWeather.WindOnly.ini`
- `CrimsonWeather.WindOnly.log` when logging is enabled

Development build:
- `CrimsonWeather.DEV.addon64`
- `CrimsonWeather.DEV.ini`
- `CrimsonWeather.DEV.log` when logging is enabled

## Startup

Crimson Weather starts automatically by default.

Set `AutoStart=0` in `CrimsonWeather.ini` if you want to start it manually from the ReShade overlay with `Start Crimson Weather`.

## Overlay

Open the ReShade overlay and use the `Crimson Weather` addon tab.

The edit scope selector is always visible at the top of the overlay. Use it to edit global preset values or per-region overrides from any tab.

Tabs and controls:
- **Presets**: load/save presets, save-as, reset sliders, edit global or per-region preset scopes, and configure the optional Time Schedule
- **General**: rain, thunder, dust, snow, force clear sky, no rain, no dust, no snow, visual time override, progress visual time, advance interval, wind, and no wind
- **Atmosphere**: Rayleigh scattering color, cloud amount, cloud height, cloud density, mid clouds, high clouds, cloud alpha, cloud phase, cloud scattering, native fog, volume fog scatter color, aerosol height, aerosol density, aerosol absorption, fog height baseline, fog height falloff, and no fog
- **Celestial**: moon texture, Milky Way texture, night sky tilt, night sky phase, sun light intensity, sun size, sun yaw/pitch lock, moon light intensity, moon size, moon yaw/pitch lock, and moon rotation
- **Experiment**: 2C, 2D, cloud variation, legacy fog, and puddle scale
- **Status**: current effective values, active hook state, startup health, and whether each value comes from global preset or a region override
- **Dev**: DEV-build-only live atmosphere lab controls

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

Moon and Milky Way texture presets save the texture name only, not a full local path. If a saved texture is missing, Crimson Weather resets that preset entry back to Native.

Animated moon texture packs use the same Moon Texture browser as static moon textures. Put animated packs under `CrimsonWeather/moon/{Pack Name}/{Moon Name}` with a `manifest.json` file and sequential frame files.

Animated moon manifest files support JSONC-style comments, frame durations, loop modes, start frame, and random start. Supported loop modes are `forward`, `pingpong`, `once`, and `hold`.

Region overrides are saved only for values that differ from the global preset. The Status tab shows which values are inherited and which are region-specific.

## Community Presets

The main addon build includes a `Community` overlay tab for browsing, searching, downloading, liking, and submitting presets through the configured community backend.

Downloaded community presets are stored separately from user-authored local presets at:

- `bin64/CrimsonWeather/community/preset`

The regular Presets tab loads both local preset `.ini` files next to the addon and community preset `.ini` files from that separate folder. Community entries are shown with a `[Community]` prefix.

Downloaded community presets can show an update action when a newer approved version is available.

## Time Schedule

The Time Schedule is global and disabled by default. It lives in `CrimsonWeather.ini`, not inside preset files.

When enabled, Crimson Weather selects presets by detected visual in-game time. Schedule entries use AM/PM time ranges and can cross midnight. Gaps are generated automatically; a gap with no preset leaves the current/manual/last scheduled preset active.

Each schedule entry can blend from the current effective state into the target preset over a real-time duration. Region overrides still apply during scheduled preset use.

Manual preset selection disables the schedule. Manually changing Visual Time Override also disables the schedule, because the scheduler and manual visual-time editing intentionally do not co-own the clock.

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
AutoSaved=0
ExtendedSliderRange=0
HotkeyToggleEffect=F10
_HotkeyOptions=F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, or single letter A-Z

[Hotkeys]
ControllerToggleEffect=dpad_down+a
_ControllerHotkeyOptions=Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back

[Preset]
LastPreset=

[TimeSchedule]
Enabled=0
EntryCount=0

[Community]
Enabled=1

[Wind]
Multiplier=1.0000
```

`AutoSaved=1` automatically saves edits to the currently selected preset shortly after the active UI interaction ends. It does not create new preset files; use `Create Preset` or `Save As` first.

WindOnly uses `CrimsonWeather.WindOnly.ini`. DEV uses `CrimsonWeather.DEV.ini`.

## Build

Open:
- `CrimsonWeather.slnx`

Build all release addon flavors:
- `.\build_release_dual.ps1`

Compiler output:
- `CrimsonWeatherReshade/x64/Release/CrimsonWeather.addon64`
- `CrimsonWeatherReshade/x64/Release/CrimsonWeather.WindOnly.addon64`
- `CrimsonWeatherReshade/x64/Release/CrimsonWeather.DEV.addon64`

## Download

- Nexus Mods: https://www.nexusmods.com/crimsondesert/mods/632
