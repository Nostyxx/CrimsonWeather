

export const UPDATE_DOWNLOAD_PAGE_URL = "https://www.nexusmods.com/crimsondesert/mods/632?tab=files";

export const UPDATE_CHANNEL = "stable";

export const UPDATE_LATEST_VERSION = "0.7.1";

export const UPDATE_ARTIFACT_MAX_BYTES = 128 * 1024 * 1024;

export const UPDATE_CHANGELOG = `Update 0.7.1
- Updated for Crimson Desert 1.10.00
Update 0.6.9
- Added Real In-Game Time controls with adjustable day and night world-time scales
- Added a safety warning before enabling real world-clock controls
- Improved the Time Schedule editor and collapsed Hook Controls by default
Update 0.6.8
Update 0.6.6
- Added toast notification configuration
- Expanded community preset controls and update handling
- Added texture browser and Time Schedule improvements
Update 0.6.5
- Updated for the latest game patch
- Added a TextureSwitcher config flag
- Fixed region detection after the latest game patch
- Fixed rain effects sometimes staying on screen after rain was disabled
Update 0.6.4
- Added V1 Animated Moon support (download the Miscellaneous file for setup info and examples)
- Added DX10 support for DDS Moon/Milky Way textures
- Added mipmap support for DDS Moon/Milky Way textures
- Added update detection for downloaded community presets
- Increased the height of the community preset browser UI
- Improved validation for unsupported DDS mip chains
- Fixed a bug where presets using the "Match In-Game Clock" setting did not apply properly when selected
- Preset upload and My Uploads sections now collapse by default
Update 0.6.3
- Added a "Match In-Game Clock" box (for use with Progress Visual Time)
- Added Cloud Visible Range slider
- Added Snow Accumulation Boundary A slider
- Added Snow Accumulation Boundary B slider
- Added Snow Coverage Threshold slider
- Moved Rain/Dust/Snow settings to their own new "Weather" tab
Update 0.6.2
- Added Community Preset browser (see below for more info)
- Fixed Visual Time Override flickering when disabling Crimson Weather
Update 0.6.1
- Added a Time Schedule system
- Added Mie Scatter Color option
- Added Cloud Flow slider
- Added Rayleigh Height slider
- Added Ozone Ratio slider
- Added Cloud Fade Range slider
- Added Cloud Detail Ratio slider
- Added config auto-saving
- Fixed a data race in slot.status
- Fixed a bug from the 0.6.0 optimization where the Advance Interval was delayed by 0.20 seconds
- Fixed a bug where the No Rain/Dust/Snow box could still produce dust
Update 0.6.0
- Fixed performance issues (hopefully)
- Added Sunlight Intensity slider
- Added Moonlight Intensity slider
- Added Cloud Alpha slider
- Added Cloud Phase Front slider
- Added Cloud Scattering Coefficient slider
- Added Rayleigh Scattering Color option
- Added Volume Fog Scatter Color option
- Added Aerosol Height slider
- Added Aerosol Density slider
- Added Aerosol Absorption slider
- Added the ability to collapse both the Milky Way and Moon texture-switcher UIs
Update 0.5.9
- Added hook control (If you experience FPS drops, try disabling the hooks you don't need in the Status tab. This is a temporary workaround until I return to fix it properly.)
- Fixed an issue where texture-switching was unavailable on some machines
Update 0.5.8
- Added Progress Visual Time box with an Advance Interval slider
- Added Extended Slider Range box
- Improved the texture-switching gate
Update 0.5.7
- Added Milky Way texture switching (now we just need someone to make textures for it :D)
- Added ability to right click sliders to directly type values
- Improved Moon/Milky Way texture switching stability
- Moon/Milky Way texture folders are now created automatically if missing
- Improved Thunder slider stability
- Code cleanup
Update 0.5.6
- Moon texture switching now also supports .png
Update 0.5.5
- Added runtime moon texture switching (see below for more info)
- Fixed region override sometimes acting as a global preset
Update 0.5.3
- Added Thunder slider
- Added No Rain box
- Added No Dust box
- Added No Snow box
- Sliders now show "NATIVE" when they are not overriding game values
- Fixed crash issues when using the Snow slider
- Improved Weather tab UI
Update 0.5.2
- Added Celestial tab
- Added Sun Size slider
- Added Sun Yaw slider
- Added Sun Pitch slider
- Added Moon Size slider
- Added Moon Yaw slider
- Added Moon Pitch slider
- Added Moon Rotation slider
- Added Night Sky Tilt slider
- Added Night Sky Phase slider
- Added No fog box
- Code cleanup
Update 0.5.1
- Updated for 1.06.00
- Added region override system (you can now set a global preset and also set different preset for each region)
- Added Auto Start config
- Added Status tab
- Improved startup flow
Update 0.5.0
- Crimson Weather is now a .addon64, an Ultimate ASI loader is no longer required. But you must now open the ReShade overlay and press "Start Crimson Weather" manually every time you start the game
- Added Cloud Amount slider (you should now be able to add clouds to scenes with no clouds)
- Added Experimental Cloud Variation slider
- Improved Cloud Height slider
- Improved UI
- Fixed presets not automatically applying
Update 0.4.2
- Updated for 1.05.00
Update 0.4.1
- Updated for 1.04.01
- Reworked the fog slider
- Improved time slider accuracy
Update 0.3.0
- Rewritten to use ReShade as the GUI overlay
Update 0.2.3
- Added Mid clouds slider
- Added High clouds slider
- Added Experiment tab
- Expanded Fog and Wind slider range
- Fixed Cloud Density no longer affecting cloud movement
- Rain now release back to native weather when set to 0
- Improved preset loading and compatibility
- Hardened AOB scanning
Update 0.2.2
- Fixed Crashing when using FSR-FG
- Fixed DualSense support
Update 0.2.1
- Fixed Crashing when using FSR-FG
- Added DualSense support
- Added Toggle Weather hotkey
Update 0.2.0
- Added presets loading and saving
- Added Show GUI on startup box
- Added Auto Apply on startup
- Added individual reset buttons
- More robust DXGI hook
- Fixed OptiScaler compatibility
Update 0.1.8
- Fixed Reset All button
- Added Cloud Density slider
- Added Cloud Height slider
- Added Controller support
- Added UI scale slider
Update 0.1.7
- Added Visual Time Override (Override game time visually)
Update 0.1.5
- Added ImGui GUI
- Added separate sliders for Rain, Snow, and Dust
- Removed Cloud sliders (until i got it working properly)
- Fixed Wind slider
- Added No wind box
- Fixed Fog slider
- Added Force Clear sky box`;
