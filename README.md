# Crimson Weather

ReShade `.addon64` weather-control mod for `CrimsonDesert.exe`.

Current stable release: `0.6.9`.

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
- Community identity and local like state in `CrimsonWeather/community/state.v1`
- Direct update install staging files may temporarily appear as `CrimsonWeather.addon64.new` and `CrimsonWeather.addon64.old`
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
- **Presets**: load/save local and downloaded community presets, save-as, reset sliders, edit global or per-region preset scopes, and configure the optional Time Schedule
- **Community**: browse, search, sort, download, like/unlike, submit, update, and delete community preset uploads
- **Favorites**: build custom sections that reuse live controls from the other tabs, with editor-driven add/remove and ordering
- **General**: real in-game time controls, day/night world-time scaling, visual time override, progress visual time, Match In-Game Clock, advance interval, wind, no wind, and optional RenoDX aurora region gating
- **Weather**: force clear sky, rain, dust, snow, thunder, no rain, no dust, no snow, snow accumulation boundaries, and snow coverage threshold
- **Atmosphere**: Rayleigh scattering color, Rayleigh height, ozone ratio, cloud amount, cloud height, cloud density, mid clouds, high clouds, cloud alpha, cloud fade range, cloud detail ratio, cloud phase, cloud scattering, cloud flow, cloud visible range, fog, no fog, volume fog scatter color, Mie scatter color, aerosol height, aerosol density, aerosol absorption, fog height baseline, and fog height falloff
- **Celestial**: static/animated moon texture, Milky Way texture, no-moon/no-Milky-Way options, night sky tilt, night sky phase, sun light intensity, sun size, sun yaw/pitch lock, moon light intensity, moon size, moon yaw/pitch lock, and moon rotation
- **Experiment**: 2C, 2D, cloud variation, legacy fog, and puddle scale
- **Status**: current effective values, active hook state, startup and update health, AutoDownload Updates, and whether each value comes from global preset or a region override

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

Moon and Milky Way texture presets save the texture name only, not a full local path. If a saved texture is missing, Crimson Weather resets that preset entry back to Native. The texture browsers also provide `No Moon` and `No Milky Way` options.

Animated moon texture packs use the same Moon Texture browser as static moon textures. Put animated packs under `CrimsonWeather/moon/{Pack Name}/{Moon Name}` with a `manifest.json` file and sequential frame files.

Animated moon manifest files support JSONC-style comments, frame durations, loop modes, start frame, and random start. Supported loop modes are `forward`, `pingpong`, `once`, and `hold`.

Static Moon and Milky Way replacements support `.png`, legacy DDS, and DX10 DDS textures. DDS is recommended for finished texture packs.

When `renodx-crimsondesert.addon64` is detected next to the Crimson Weather addon, the General tab shows optional RenoDX interaction controls. Allowed Aurora Regions is disabled by default, can gate RenoDX aurora brightness by region, and is saved only in presets that include it.

Region overrides are saved only for values that differ from the global preset. The Status tab shows which values are inherited and which are region-specific.

## Community Presets

The main addon build includes a `Community` overlay tab for browsing, searching, sorting, downloading, liking, and submitting presets through the configured community backend. The My Uploads section lets the submitting install delete uploads and submit updates once an upload is no longer pending.

Downloaded community presets are stored separately from user-authored local presets at:

- `bin64/CrimsonWeather/community/preset`

The regular Presets tab loads both local preset `.ini` files next to the addon and community preset `.ini` files from that separate folder. Community entries are shown with a `[Community]` prefix.

Downloaded community presets can show an update action when a newer approved version is available.

Community access is anonymous and identified locally by `CrimsonWeather/community/state.v1`. Keep this file if you want this installation to continue recognizing its uploads and likes. Setting `[Community] Enabled=0` disables community network activity.

## Time Schedule

The Time Schedule is global and disabled by default. It lives in `CrimsonWeather.ini`, not inside preset files.

When enabled, Crimson Weather selects presets using either the visual time override clock or the detected in-game HUD clock, selectable in the Time Schedule UI. Schedule entries use AM/PM time ranges and can cross midnight. Gaps are generated automatically; a gap with no preset leaves the current/manual/last scheduled preset active.

Each schedule entry can blend from the current effective state into the target preset over a real-time duration. Region overrides still apply during scheduled preset use.

Manual preset selection disables the schedule. Manually changing Visual Time Override also disables the schedule, because the scheduler and manual visual-time editing intentionally do not co-own the clock.

## Real In-Game Time

Real In-Game Time controls the game's actual world clock. Adjusting it can affect lighting, NPC behavior, quests, and other timed game systems, so the overlay shows a warning before its controls are first selected in each session.

The shared Time control selector chooses whether the clock dial edits real in-game time or visual time override. Real in-game time includes `-1 Day` and `+1 Day` actions plus separate Day Time Scale and Night Time Scale controls. `NATIVE` is the normal game speed.

Day Time Scale applies from `03:00 AM` through `06:59 PM`. Night Time Scale applies from `07:00 PM` through `02:59 AM`. Custom time scaling continues while Visual Time Override is selected, allowing visual screenshot changes without returning world time to native speed.

Real in-game time controls are global only and are not available in region overrides or saved inside presets. Day and night scale values are stored locally in `CrimsonWeather.ini`.

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
ToastNotification=1
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
TimeSource=VisualTimeOverride
EntryCount=0

[Community]
Enabled=1

[Updater]
Enabled=1
AutoDownload=0

[TextureSwitcher]
Enabled=1

[RealGameTime]
DayScale=1.0000
NightScale=1.0000
```

`AutoSaved=1` automatically saves edits to the currently selected preset shortly after the active UI interaction ends. It does not create new preset files; use `Create Preset` or `Save As` first.

`ToastNotification=0` disables the in-game toast messages shown by Crimson Weather. `TextureSwitcher` and `Updater` can also be disabled independently by setting their `Enabled` value to `0`. `Updater AutoDownload=1` changes the update button from opening Nexus Mods to installing the downloaded `.addon64` directly into the game `bin64` folder; the new add-on is used after restarting Crimson Desert.

WindOnly uses `CrimsonWeather.WindOnly.ini` and stores its wind multiplier under `[Wind]`. DEV uses `CrimsonWeather.DEV.ini`.

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
