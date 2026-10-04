// Captured from pre-refactor commit 24b2a9b503c528c3025e2cfb3274b80fe681dc7c.
// HTML bodies use SHA-256 to preserve exact-template coverage without duplicating pages.
export const routeBaseline = [
  {
    "method": "GET",
    "path": "/missing",
    "expected": {
      "status": 404,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Not found.\"}"
    }
  },
  {
    "method": "POST",
    "path": "/missing",
    "expected": {
      "status": 404,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Not found.\"}"
    }
  },
  {
    "method": "OPTIONS",
    "path": "/api/v1/catalog",
    "expected": {
      "status": 404,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Not found.\"}"
    }
  },
  {
    "method": "GET",
    "path": "/api/v1/update?version=0.7.6",
    "expected": {
      "status": 200,
      "headers": [
        [
          "cache-control",
          "public, max-age=300"
        ],
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":true,\"channel\":\"stable\",\"currentVersion\":\"0.7.6\",\"updateAvailable\":false,\"version\":\"0.7.1\",\"title\":\"Crimson Weather 0.7.1\",\"changelog\":\"Update 0.7.1\\n- Updated for Crimson Desert 1.10.00\\nUpdate 0.6.9\\n- Added Real In-Game Time controls with adjustable day and night world-time scales\\n- Added a safety warning before enabling real world-clock controls\\n- Improved the Time Schedule editor and collapsed Hook Controls by default\\nUpdate 0.6.8\\nUpdate 0.6.6\\n- Added toast notification configuration\\n- Expanded community preset controls and update handling\\n- Added texture browser and Time Schedule improvements\\nUpdate 0.6.5\\n- Updated for the latest game patch\\n- Added a TextureSwitcher config flag\\n- Fixed region detection after the latest game patch\\n- Fixed rain effects sometimes staying on screen after rain was disabled\\nUpdate 0.6.4\\n- Added V1 Animated Moon support (download the Miscellaneous file for setup info and examples)\\n- Added DX10 support for DDS Moon/Milky Way textures\\n- Added mipmap support for DDS Moon/Milky Way textures\\n- Added update detection for downloaded community presets\\n- Increased the height of the community preset browser UI\\n- Improved validation for unsupported DDS mip chains\\n- Fixed a bug where presets using the \\\"Match In-Game Clock\\\" setting did not apply properly when selected\\n- Preset upload and My Uploads sections now collapse by default\\nUpdate 0.6.3\\n- Added a \\\"Match In-Game Clock\\\" box (for use with Progress Visual Time)\\n- Added Cloud Visible Range slider\\n- Added Snow Accumulation Boundary A slider\\n- Added Snow Accumulation Boundary B slider\\n- Added Snow Coverage Threshold slider\\n- Moved Rain/Dust/Snow settings to their own new \\\"Weather\\\" tab\\nUpdate 0.6.2\\n- Added Community Preset browser (see below for more info)\\n- Fixed Visual Time Override flickering when disabling Crimson Weather\\nUpdate 0.6.1\\n- Added a Time Schedule system\\n- Added Mie Scatter Color option\\n- Added Cloud Flow slider\\n- Added Rayleigh Height slider\\n- Added Ozone Ratio slider\\n- Added Cloud Fade Range slider\\n- Added Cloud Detail Ratio slider\\n- Added config auto-saving\\n- Fixed a data race in slot.status\\n- Fixed a bug from the 0.6.0 optimization where the Advance Interval was delayed by 0.20 seconds\\n- Fixed a bug where the No Rain/Dust/Snow box could still produce dust\\nUpdate 0.6.0\\n- Fixed performance issues (hopefully)\\n- Added Sunlight Intensity slider\\n- Added Moonlight Intensity slider\\n- Added Cloud Alpha slider\\n- Added Cloud Phase Front slider\\n- Added Cloud Scattering Coefficient slider\\n- Added Rayleigh Scattering Color option\\n- Added Volume Fog Scatter Color option\\n- Added Aerosol Height slider\\n- Added Aerosol Density slider\\n- Added Aerosol Absorption slider\\n- Added the ability to collapse both the Milky Way and Moon texture-switcher UIs\\nUpdate 0.5.9\\n- Added hook control (If you experience FPS drops, try disabling the hooks you don't need in the Status tab. This is a temporary workaround until I return to fix it properly.)\\n- Fixed an issue where texture-switching was unavailable on some machines\\nUpdate 0.5.8\\n- Added Progress Visual Time box with an Advance Interval slider\\n- Added Extended Slider Range box\\n- Improved the texture-switching gate\\nUpdate 0.5.7\\n- Added Milky Way texture switching (now we just need someone to make textures for it :D)\\n- Added ability to right click sliders to directly type values\\n- Improved Moon/Milky Way texture switching stability\\n- Moon/Milky Way texture folders are now created automatically if missing\\n- Improved Thunder slider stability\\n- Code cleanup\\nUpdate 0.5.6\\n- Moon texture switching now also supports .png\\nUpdate 0.5.5\\n- Added runtime moon texture switching (see below for more info)\\n- Fixed region override sometimes acting as a global preset\\nUpdate 0.5.3\\n- Added Thunder slider\\n- Added No Rain box\\n- Added No Dust box\\n- Added No Snow box\\n- Sliders now show \\\"NATIVE\\\" when they are not overriding game values\\n- Fixed crash issues when using the Snow slider\\n- Improved Weather tab UI\\nUpdate 0.5.2\\n- Added Celestial tab\\n- Added Sun Size slider\\n- Added Sun Yaw slider\\n- Added Sun Pitch slider\\n- Added Moon Size slider\\n- Added Moon Yaw slider\\n- Added Moon Pitch slider\\n- Added Moon Rotation slider\\n- Added Night Sky Tilt slider\\n- Added Night Sky Phase slider\\n- Added No fog box\\n- Code cleanup\\nUpdate 0.5.1\\n- Updated for 1.06.00\\n- Added region override system (you can now set a global preset and also set different preset for each region)\\n- Added Auto Start config\\n- Added Status tab\\n- Improved startup flow\\nUpdate 0.5.0\\n- Crimson Weather is now a .addon64, an Ultimate ASI loader is no longer required. But you must now open the ReShade overlay and press \\\"Start Crimson Weather\\\" manually every time you start the game\\n- Added Cloud Amount slider (you should now be able to add clouds to scenes with no clouds)\\n- Added Experimental Cloud Variation slider\\n- Improved Cloud Height slider\\n- Improved UI\\n- Fixed presets not automatically applying\\nUpdate 0.4.2\\n- Updated for 1.05.00\\nUpdate 0.4.1\\n- Updated for 1.04.01\\n- Reworked the fog slider\\n- Improved time slider accuracy\\nUpdate 0.3.0\\n- Rewritten to use ReShade as the GUI overlay\\nUpdate 0.2.3\\n- Added Mid clouds slider\\n- Added High clouds slider\\n- Added Experiment tab\\n- Expanded Fog and Wind slider range\\n- Fixed Cloud Density no longer affecting cloud movement\\n- Rain now release back to native weather when set to 0\\n- Improved preset loading and compatibility\\n- Hardened AOB scanning\\nUpdate 0.2.2\\n- Fixed Crashing when using FSR-FG\\n- Fixed DualSense support\\nUpdate 0.2.1\\n- Fixed Crashing when using FSR-FG\\n- Added DualSense support\\n- Added Toggle Weather hotkey\\nUpdate 0.2.0\\n- Added presets loading and saving\\n- Added Show GUI on startup box\\n- Added Auto Apply on startup\\n- Added individual reset buttons\\n- More robust DXGI hook\\n- Fixed OptiScaler compatibility\\nUpdate 0.1.8\\n- Fixed Reset All button\\n- Added Cloud Density slider\\n- Added Cloud Height slider\\n- Added Controller support\\n- Added UI scale slider\\nUpdate 0.1.7\\n- Added Visual Time Override (Override game time visually)\\nUpdate 0.1.5\\n- Added ImGui GUI\\n- Added separate sliders for Rain, Snow, and Dust\\n- Removed Cloud sliders (until i got it working properly)\\n- Fixed Wind slider\\n- Added No wind box\\n- Fixed Fog slider\\n- Added Force Clear sky box\",\"downloadPageUrl\":\"https://www.nexusmods.com/crimsondesert/mods/632?tab=files\",\"addonDownloadUrl\":\"\",\"addonSha256\":\"\",\"addonSizeBytes\":0,\"publishedAt\":\"\",\"critical\":false}"
    }
  },
  {
    "method": "GET",
    "path": "/api/v1/update?version=0.6.0",
    "expected": {
      "status": 200,
      "headers": [
        [
          "cache-control",
          "public, max-age=300"
        ],
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":true,\"channel\":\"stable\",\"currentVersion\":\"0.6.0\",\"updateAvailable\":true,\"version\":\"0.7.1\",\"title\":\"Crimson Weather 0.7.1\",\"changelog\":\"Update 0.7.1\\n- Updated for Crimson Desert 1.10.00\\nUpdate 0.6.9\\n- Added Real In-Game Time controls with adjustable day and night world-time scales\\n- Added a safety warning before enabling real world-clock controls\\n- Improved the Time Schedule editor and collapsed Hook Controls by default\\nUpdate 0.6.8\\nUpdate 0.6.6\\n- Added toast notification configuration\\n- Expanded community preset controls and update handling\\n- Added texture browser and Time Schedule improvements\\nUpdate 0.6.5\\n- Updated for the latest game patch\\n- Added a TextureSwitcher config flag\\n- Fixed region detection after the latest game patch\\n- Fixed rain effects sometimes staying on screen after rain was disabled\\nUpdate 0.6.4\\n- Added V1 Animated Moon support (download the Miscellaneous file for setup info and examples)\\n- Added DX10 support for DDS Moon/Milky Way textures\\n- Added mipmap support for DDS Moon/Milky Way textures\\n- Added update detection for downloaded community presets\\n- Increased the height of the community preset browser UI\\n- Improved validation for unsupported DDS mip chains\\n- Fixed a bug where presets using the \\\"Match In-Game Clock\\\" setting did not apply properly when selected\\n- Preset upload and My Uploads sections now collapse by default\\nUpdate 0.6.3\\n- Added a \\\"Match In-Game Clock\\\" box (for use with Progress Visual Time)\\n- Added Cloud Visible Range slider\\n- Added Snow Accumulation Boundary A slider\\n- Added Snow Accumulation Boundary B slider\\n- Added Snow Coverage Threshold slider\\n- Moved Rain/Dust/Snow settings to their own new \\\"Weather\\\" tab\\nUpdate 0.6.2\\n- Added Community Preset browser (see below for more info)\\n- Fixed Visual Time Override flickering when disabling Crimson Weather\\nUpdate 0.6.1\\n- Added a Time Schedule system\\n- Added Mie Scatter Color option\\n- Added Cloud Flow slider\\n- Added Rayleigh Height slider\\n- Added Ozone Ratio slider\\n- Added Cloud Fade Range slider\\n- Added Cloud Detail Ratio slider\\n- Added config auto-saving\\n- Fixed a data race in slot.status\\n- Fixed a bug from the 0.6.0 optimization where the Advance Interval was delayed by 0.20 seconds\\n- Fixed a bug where the No Rain/Dust/Snow box could still produce dust\\nUpdate 0.6.0\\n- Fixed performance issues (hopefully)\\n- Added Sunlight Intensity slider\\n- Added Moonlight Intensity slider\\n- Added Cloud Alpha slider\\n- Added Cloud Phase Front slider\\n- Added Cloud Scattering Coefficient slider\\n- Added Rayleigh Scattering Color option\\n- Added Volume Fog Scatter Color option\\n- Added Aerosol Height slider\\n- Added Aerosol Density slider\\n- Added Aerosol Absorption slider\\n- Added the ability to collapse both the Milky Way and Moon texture-switcher UIs\\nUpdate 0.5.9\\n- Added hook control (If you experience FPS drops, try disabling the hooks you don't need in the Status tab. This is a temporary workaround until I return to fix it properly.)\\n- Fixed an issue where texture-switching was unavailable on some machines\\nUpdate 0.5.8\\n- Added Progress Visual Time box with an Advance Interval slider\\n- Added Extended Slider Range box\\n- Improved the texture-switching gate\\nUpdate 0.5.7\\n- Added Milky Way texture switching (now we just need someone to make textures for it :D)\\n- Added ability to right click sliders to directly type values\\n- Improved Moon/Milky Way texture switching stability\\n- Moon/Milky Way texture folders are now created automatically if missing\\n- Improved Thunder slider stability\\n- Code cleanup\\nUpdate 0.5.6\\n- Moon texture switching now also supports .png\\nUpdate 0.5.5\\n- Added runtime moon texture switching (see below for more info)\\n- Fixed region override sometimes acting as a global preset\\nUpdate 0.5.3\\n- Added Thunder slider\\n- Added No Rain box\\n- Added No Dust box\\n- Added No Snow box\\n- Sliders now show \\\"NATIVE\\\" when they are not overriding game values\\n- Fixed crash issues when using the Snow slider\\n- Improved Weather tab UI\\nUpdate 0.5.2\\n- Added Celestial tab\\n- Added Sun Size slider\\n- Added Sun Yaw slider\\n- Added Sun Pitch slider\\n- Added Moon Size slider\\n- Added Moon Yaw slider\\n- Added Moon Pitch slider\\n- Added Moon Rotation slider\\n- Added Night Sky Tilt slider\\n- Added Night Sky Phase slider\\n- Added No fog box\\n- Code cleanup\\nUpdate 0.5.1\\n- Updated for 1.06.00\\n- Added region override system (you can now set a global preset and also set different preset for each region)\\n- Added Auto Start config\\n- Added Status tab\\n- Improved startup flow\\nUpdate 0.5.0\\n- Crimson Weather is now a .addon64, an Ultimate ASI loader is no longer required. But you must now open the ReShade overlay and press \\\"Start Crimson Weather\\\" manually every time you start the game\\n- Added Cloud Amount slider (you should now be able to add clouds to scenes with no clouds)\\n- Added Experimental Cloud Variation slider\\n- Improved Cloud Height slider\\n- Improved UI\\n- Fixed presets not automatically applying\\nUpdate 0.4.2\\n- Updated for 1.05.00\\nUpdate 0.4.1\\n- Updated for 1.04.01\\n- Reworked the fog slider\\n- Improved time slider accuracy\\nUpdate 0.3.0\\n- Rewritten to use ReShade as the GUI overlay\\nUpdate 0.2.3\\n- Added Mid clouds slider\\n- Added High clouds slider\\n- Added Experiment tab\\n- Expanded Fog and Wind slider range\\n- Fixed Cloud Density no longer affecting cloud movement\\n- Rain now release back to native weather when set to 0\\n- Improved preset loading and compatibility\\n- Hardened AOB scanning\\nUpdate 0.2.2\\n- Fixed Crashing when using FSR-FG\\n- Fixed DualSense support\\nUpdate 0.2.1\\n- Fixed Crashing when using FSR-FG\\n- Added DualSense support\\n- Added Toggle Weather hotkey\\nUpdate 0.2.0\\n- Added presets loading and saving\\n- Added Show GUI on startup box\\n- Added Auto Apply on startup\\n- Added individual reset buttons\\n- More robust DXGI hook\\n- Fixed OptiScaler compatibility\\nUpdate 0.1.8\\n- Fixed Reset All button\\n- Added Cloud Density slider\\n- Added Cloud Height slider\\n- Added Controller support\\n- Added UI scale slider\\nUpdate 0.1.7\\n- Added Visual Time Override (Override game time visually)\\nUpdate 0.1.5\\n- Added ImGui GUI\\n- Added separate sliders for Rain, Snow, and Dust\\n- Removed Cloud sliders (until i got it working properly)\\n- Fixed Wind slider\\n- Added No wind box\\n- Fixed Fog slider\\n- Added Force Clear sky box\",\"downloadPageUrl\":\"https://www.nexusmods.com/crimsondesert/mods/632?tab=files\",\"addonDownloadUrl\":\"\",\"addonSha256\":\"\",\"addonSizeBytes\":0,\"publishedAt\":\"\",\"critical\":false}"
    }
  },
  {
    "method": "GET",
    "path": "/api/v1/update/artifact",
    "expected": {
      "status": 404,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Update artifact is not configured.\"}"
    }
  },
  {
    "method": "GET",
    "path": "/api/v1/update/artifact?version=0.0.1",
    "expected": {
      "status": 404,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Requested update artifact version is not available.\"}"
    }
  },
  {
    "method": "GET",
    "path": "/api/v1/me/presets",
    "expected": {
      "status": 400,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Missing anonymous client id.\"}"
    }
  },
  {
    "method": "DELETE",
    "path": "/api/v1/me/presets/test",
    "expected": {
      "status": 400,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Missing anonymous client id.\"}"
    }
  },
  {
    "method": "PUT",
    "path": "/api/v1/me/presets/test",
    "expected": {
      "status": 400,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Missing anonymous client id.\"}"
    }
  },
  {
    "method": "DELETE",
    "path": "/api/v1/me/presets/test/update",
    "expected": {
      "status": 400,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Missing anonymous client id.\"}"
    }
  },
  {
    "method": "POST",
    "path": "/api/v1/presets",
    "body": "{}",
    "expected": {
      "status": 400,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Missing anonymous client id.\"}"
    }
  },
  {
    "method": "POST",
    "path": "/api/v1/presets",
    "body": "bad",
    "expected": {
      "status": 400,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Invalid JSON.\"}"
    }
  },
  {
    "method": "GET",
    "path": "/api/v1/presets/test/download",
    "expected": {
      "status": 404,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Preset not found.\"}"
    }
  },
  {
    "method": "POST",
    "path": "/api/v1/presets/test/like",
    "expected": {
      "status": 404,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Preset not found.\"}"
    }
  },
  {
    "method": "GET",
    "path": "/admin",
    "expected": {
      "status": 404,
      "headers": [
        [
          "content-type",
          "text/plain; charset=utf-8"
        ]
      ],
      "body": "Not found."
    }
  },
  {
    "method": "GET",
    "path": "/admin?key=review-key",
    "expected": {
      "status": 200,
      "headers": [
        [
          "content-type",
          "text/html; charset=utf-8"
        ]
      ],
      "bodySha256": "8f95bb860210c29ec564481eb135a4a81a5333faf7115204c6b9af0a5b62a5e7"
    }
  },
  {
    "method": "GET",
    "path": "/admin",
    "headers": {
      "authorization": "Bearer review-token"
    },
    "expected": {
      "status": 200,
      "headers": [
        [
          "content-type",
          "text/html; charset=utf-8"
        ]
      ],
      "bodySha256": "710cb9b9d80359e7c38e0c6c8b58350d2542cb117e1a277bec43155da09157ec"
    }
  },
  {
    "method": "POST",
    "path": "/api/v1/admin/login",
    "body": "{}",
    "expected": {
      "status": 404,
      "headers": [
        [
          "content-type",
          "text/plain; charset=utf-8"
        ]
      ],
      "body": "Not found."
    }
  },
  {
    "method": "POST",
    "path": "/api/v1/admin/login?key=review-key",
    "body": "{\"token\":\"wrong\"}",
    "expected": {
      "status": 401,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Invalid admin token.\"}"
    }
  },
  {
    "method": "POST",
    "path": "/api/v1/admin/logout",
    "expected": {
      "status": 200,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ],
        [
          "set-cookie",
          "cw_admin_session=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0"
        ]
      ],
      "body": "{\"ok\":true}"
    }
  },
  {
    "method": "GET",
    "path": "/api/v1/admin/presets",
    "expected": {
      "status": 401,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Unauthorized\"}"
    }
  },
  {
    "method": "GET",
    "path": "/api/v1/admin/not-real",
    "expected": {
      "status": 401,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Unauthorized\"}"
    }
  },
  {
    "method": "GET",
    "path": "/api/v1/admin/not-real",
    "headers": {
      "authorization": "Bearer review-token"
    },
    "expected": {
      "status": 404,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":false,\"error\":\"Not found.\"}"
    }
  },
  {
    "method": "GET",
    "path": "/api/v1/admin/presets",
    "headers": {
      "authorization": "Bearer review-token"
    },
    "expected": {
      "status": 200,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":true,\"presets\":[]}"
    }
  },
  {
    "method": "GET",
    "path": "/api/v1/admin/whitelist",
    "headers": {
      "authorization": "Bearer review-token"
    },
    "expected": {
      "status": 200,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":true,\"clients\":[]}"
    }
  },
  {
    "method": "GET",
    "path": "/api/v1/admin/audit",
    "headers": {
      "authorization": "Bearer review-token"
    },
    "expected": {
      "status": 200,
      "headers": [
        [
          "content-type",
          "application/json; charset=utf-8"
        ]
      ],
      "body": "{\"ok\":true,\"audit\":[]}"
    }
  }
];
