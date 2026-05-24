const PRESET_HEADER = "[CrimsonWeatherPreset]";
const CURRENT_FORMAT_VERSION = 6;
const CATALOG_KEY = "catalog/catalog.v1.json";
const UPDATE_DOWNLOAD_PAGE_URL = "https://www.nexusmods.com/crimsondesert/mods/632?tab=files";
const UPDATE_CHANNEL = "stable";
const UPDATE_LATEST_VERSION = "0.6.8";
const UPDATE_ARTIFACT_MAX_BYTES = 128 * 1024 * 1024;
const UPDATE_CHANGELOG = `Update 0.6.8
- Added RenoDX aurora region gating support
- Added community preset upload validation for RenoDX aurora preset fields
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
const ALLOWED_SECTIONS = new Set([
  "Meta", "Weather", "Time", "Cloud", "Experiment", "Celestial", "Atmosphere", "RenoDX"
]);
const ALLOWED_KEYS = new Set([
  "FormatVersion", "Enabled",
  "ForceClearSky", "NoRain", "Rain", "Thunder", "NoDust", "Dust", "NoSnow", "Snow",
  "SnowAccumBoundaryAEnabled", "SnowAccumBoundaryA", "SnowAccumBoundaryBEnabled", "SnowAccumBoundaryB",
  "SnowCoverageThresholdEnabled", "SnowCoverageThreshold",
  "VisualTimeOverride", "ProgressVisualTime", "ProgressVisualTimeMatchGameTime", "ProgressVisualTimeIntervalMs", "TimeHour",
  "CloudAmountEnabled", "CloudAmount", "CloudHeightEnabled", "CloudHeight",
  "CloudDensityEnabled", "CloudDensity", "MidCloudsEnabled", "MidClouds",
  "HighCloudLayerEnabled", "HighCloudLayer", "CloudAlphaEnabled", "CloudAlpha",
  "CloudFadeRangeEnabled", "CloudFadeRange", "CloudDetailRatioEnabled", "CloudDetailRatio",
  "CloudPhaseFrontEnabled", "CloudPhaseFront", "CloudScatteringCoefficientEnabled",
  "CloudScatteringCoefficient", "CloudFlowEnabled", "CloudFlow", "CloudVisibleRangeEnabled",
  "CloudVisibleRange", "RayleighHeightEnabled",
  "RayleighHeight", "OzoneRatioEnabled", "OzoneRatio", "RayleighScatteringColorEnabled",
  "RayleighScatteringColorR", "RayleighScatteringColorG", "RayleighScatteringColorB",
  "2CEnabled", "2C", "2DEnabled", "2D", "CloudVariationEnabled", "CloudVariation",
  "PuddleScaleEnabled", "PuddleScale",
  "NightSkyTiltEnabled", "NightSkyTilt", "NightSkyPhaseEnabled", "NightSkyPhase",
  "SunSizeEnabled", "SunSize", "SunLightIntensityEnabled", "SunLightIntensity",
  "SunYawEnabled", "SunYaw", "SunPitchEnabled", "SunPitch", "MoonSizeEnabled",
  "MoonSize", "MoonLightIntensityEnabled", "MoonLightIntensity", "MoonYawEnabled",
  "MoonYaw", "MoonPitchEnabled", "MoonPitch", "MoonRollEnabled", "MoonRoll",
  "MoonTextureEnabled", "MoonTexture", "MilkywayTextureEnabled", "MilkywayTexture",
  "FogEnabled", "Fog", "NativeFogEnabled", "NativeFog", "VolumeFogScatterColorEnabled",
  "VolumeFogScatterColorR", "VolumeFogScatterColorG", "VolumeFogScatterColorB",
  "VolumeFogScatterColorA", "MieScatterColorEnabled", "MieScatterColorR",
  "MieScatterColorG", "MieScatterColorB", "MieScatterColorA", "MieScaleHeightEnabled",
  "MieScaleHeight", "MieAerosolDensityEnabled", "MieAerosolDensity",
  "MieAerosolAbsorptionEnabled", "MieAerosolAbsorption", "HeightFogBaselineEnabled",
  "HeightFogBaseline", "HeightFogFalloffEnabled", "HeightFogFalloff", "NoFog", "Wind", "NoWind",
  "AuroraEnabled", "AuroraGateEnabled", "RenoDxAuroraEnabled",
  "AuroraRegionMask", "RenoDxAuroraRegionMask", "RenoDXAuroraRegionMask"
]);
const STRING_KEYS = new Set(["MoonTexture", "MilkywayTexture"]);

function json(body, status = 200, headers = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", ...headers }
  });
}

function text(body, status = 200, headers = {}) {
  return new Response(body, { status, headers: { "content-type": "text/plain; charset=utf-8", ...headers } });
}

function nowIso() {
  return new Date().toISOString();
}

function addDaysIso(days) {
  return new Date(Date.now() + days * 24 * 60 * 60 * 1000).toISOString();
}

function bad(message, status = 400, details = undefined) {
  return json({ ok: false, error: message, details }, status);
}

function normalizeVersion(value) {
  return String(value || "").trim().replace(/^v/i, "").replace(/\s+.*$/, "");
}

function compareVersions(a, b) {
  const aa = normalizeVersion(a).split(".").map((part) => Number.parseInt(part, 10) || 0);
  const bb = normalizeVersion(b).split(".").map((part) => Number.parseInt(part, 10) || 0);
  const count = Math.max(aa.length, bb.length, 3);
  for (let i = 0; i < count; i++) {
    const av = aa[i] || 0;
    const bv = bb[i] || 0;
    if (av !== bv) return av > bv ? 1 : -1;
  }
  return 0;
}

function defaultUpdateSettings(env) {
  return {
    channel: UPDATE_CHANNEL,
    latestVersion: normalizeVersion(env.UPDATE_LATEST_VERSION || UPDATE_LATEST_VERSION),
    downloadPageUrl: sanitizeText(env.UPDATE_DOWNLOAD_PAGE_URL || UPDATE_DOWNLOAD_PAGE_URL, 300),
    addonR2Key: sanitizeText(env.UPDATE_ADDON_R2_KEY || "", 300),
    addonSha256: sanitizeText(env.UPDATE_ADDON_SHA256 || "", 80).toLowerCase(),
    addonSizeBytes: Number(env.UPDATE_ADDON_SIZE_BYTES || 0) || 0,
    changelog: String(env.UPDATE_CHANGELOG || UPDATE_CHANGELOG),
    publishedAt: String(env.UPDATE_PUBLISHED_AT || ""),
    critical: String(env.UPDATE_CRITICAL || "0") === "1",
    source: "default"
  };
}

async function getAppSetting(env, key) {
  try {
    const row = await env.DB.prepare("SELECT value FROM app_settings WHERE key=?").bind(key).first();
    return row ? String(row.value || "") : "";
  } catch {
    return "";
  }
}

async function setAppSetting(env, request, key, value) {
  await env.DB.prepare(
    "INSERT INTO app_settings (key,value,updated_at,updated_by) VALUES (?,?,?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at, updated_by=excluded.updated_by"
  ).bind(key, String(value ?? ""), nowIso(), adminIdentity(request, env)).run();
}

async function getUpdateSettings(env) {
  const settings = defaultUpdateSettings(env);
  const rows = await Promise.all([
    getAppSetting(env, "update.latestVersion"),
    getAppSetting(env, "update.downloadPageUrl"),
    getAppSetting(env, "update.changelog"),
    getAppSetting(env, "update.publishedAt"),
    getAppSetting(env, "update.critical"),
    getAppSetting(env, "update.addonR2Key"),
    getAppSetting(env, "update.addonSha256"),
    getAppSetting(env, "update.addonSizeBytes")
  ]);
  if (rows.some((value) => value !== "")) settings.source = "database";
  if (rows[0]) settings.latestVersion = normalizeVersion(rows[0]);
  if (rows[1]) settings.downloadPageUrl = sanitizeText(rows[1], 300);
  if (rows[2]) settings.changelog = String(rows[2]);
  if (rows[3]) settings.publishedAt = String(rows[3]).slice(0, 80);
  if (rows[4]) settings.critical = rows[4] === "1" || rows[4] === "true";
  if (rows[5]) settings.addonR2Key = sanitizeText(rows[5], 300);
  if (rows[6]) settings.addonSha256 = sanitizeText(rows[6], 80).toLowerCase();
  if (rows[7]) settings.addonSizeBytes = Number(rows[7]) || 0;
  return settings;
}

async function updateInfo(request, env) {
  const url = new URL(request.url);
  const channel = sanitizeText(url.searchParams.get("channel") || request.headers.get("x-cw-channel") || UPDATE_CHANNEL, 20) || UPDATE_CHANNEL;
  const currentVersion = normalizeVersion(url.searchParams.get("version") || request.headers.get("x-cw-client-version") || "");
  const settings = await getUpdateSettings(env);
  const latestVersion = settings.latestVersion;
  const updateAvailable = currentVersion ? compareVersions(latestVersion, currentVersion) > 0 : true;
  const hasAddonArtifact = !!(settings.addonR2Key && /^[a-f0-9]{64}$/i.test(settings.addonSha256) && settings.addonSizeBytes > 0);
  return json({
    ok: true,
    channel,
    currentVersion,
    updateAvailable,
    version: latestVersion,
    title: `Crimson Weather ${latestVersion}`,
    changelog: settings.changelog,
    downloadPageUrl: settings.downloadPageUrl,
    addonDownloadUrl: hasAddonArtifact ? `${url.origin}/api/v1/update/artifact?version=${encodeURIComponent(latestVersion)}` : "",
    addonSha256: hasAddonArtifact ? settings.addonSha256 : "",
    addonSizeBytes: hasAddonArtifact ? settings.addonSizeBytes : 0,
    publishedAt: settings.publishedAt,
    critical: settings.critical
  }, 200, {
    "cache-control": "public, max-age=300"
  });
}

async function updateArtifact(request, env) {
  const url = new URL(request.url);
  const settings = await getUpdateSettings(env);
  const requestedVersion = normalizeVersion(url.searchParams.get("version") || "");
  if (requestedVersion && requestedVersion !== settings.latestVersion) {
    return bad("Requested update artifact version is not available.", 404);
  }
  if (!settings.addonR2Key || !/^[a-f0-9]{64}$/i.test(settings.addonSha256)) {
    return bad("Update artifact is not configured.", 404);
  }
  const object = await env.PRESETS.get(settings.addonR2Key);
  if (!object) return bad("Update artifact was not found.", 404);
  return new Response(object.body, {
    status: 200,
    headers: {
      "content-type": "application/octet-stream",
      "content-disposition": `attachment; filename="CrimsonWeather-${settings.latestVersion}.addon64"`,
      "x-cw-addon-sha256": settings.addonSha256,
      "x-cw-addon-size-bytes": String(settings.addonSizeBytes || ""),
      "cache-control": "no-store"
    }
  });
}

function sanitizeText(value, maxLen) {
  return String(value ?? "").replace(/[\u0000-\u001f\u007f]/g, " ").replace(/\s+/g, " ").trim().slice(0, maxLen);
}

function slugify(value) {
  return sanitizeText(value, 80).toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 48) || "preset";
}

function bytesOf(textValue) {
  return new TextEncoder().encode(textValue);
}

async function sha256Hex(data) {
  const bytes = typeof data === "string" ? bytesOf(data) : data;
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function hmacHex(secret, value) {
  if (!value) return "";
  const key = await crypto.subtle.importKey("raw", bytesOf(secret || "dev-secret"), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const sig = await crypto.subtle.sign("HMAC", key, bytesOf(value));
  return [...new Uint8Array(sig)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function deviceHashSecret(env) {
  return env.DEVICE_HASH_SECRET || env.ADMIN_TOKEN || "dev-secret";
}

async function hashClientId(env, clientId) {
  return hmacHex(deviceHashSecret(env), clientId);
}

function clientFingerprint(submitterHash) {
  return submitterHash ? `${submitterHash.slice(0, 12)}...${submitterHash.slice(-6)}` : "";
}

function withClientFingerprint(row) {
  return { ...row, client_fingerprint: clientFingerprint(row?.submitter_hash || "") };
}

function normalizeLimit(value, fallback = 100, max = 500) {
  const parsed = Number.parseInt(value || "", 10);
  if (!Number.isFinite(parsed) || parsed <= 0) return fallback;
  return Math.min(parsed, max);
}

function likeTerm(value) {
  const clean = sanitizeText(value, 120);
  return clean ? `%${clean}%` : "";
}

async function submitterHashFromRequest(request, env) {
  const clientId = request.headers.get("x-cw-client-id") || "";
  if (!clientId || clientId.length > 128) return "";
  return hashClientId(env, clientId);
}

async function whitelistRow(env, submitterHash) {
  if (!submitterHash) return null;
  return env.DB.prepare("SELECT * FROM client_whitelist WHERE submitter_hash=? AND auto_approve=1").bind(submitterHash).first();
}

function extractFormatVersion(lines) {
  for (const line of lines) {
    const eq = line.indexOf("=");
    if (eq < 0) continue;
    if (line.slice(0, eq).trim().toLowerCase() === "formatversion") {
      const parsed = Number.parseInt(line.slice(eq + 1).trim(), 10);
      return Number.isFinite(parsed) ? parsed : 0;
    }
  }
  return 0;
}

export function scanPresetIni(iniText, maxBytes = 65536) {
  const result = { ok: true, errors: [], warnings: [], formatVersion: 0 };
  if (typeof iniText !== "string" || !iniText.trim()) {
    result.ok = false;
    result.errors.push("Preset text is empty.");
    return result;
  }
  const byteLength = bytesOf(iniText).byteLength;
  if (byteLength > maxBytes) result.errors.push(`Preset exceeds ${maxBytes} bytes.`);
  if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/.test(iniText)) result.errors.push("Preset contains control characters.");
  const lines = iniText.replace(/^\uFEFF/, "").split(/\r?\n/).map((line) => line.trim());
  if (lines[0] !== PRESET_HEADER) result.errors.push("Missing [CrimsonWeatherPreset] header.");
  result.formatVersion = extractFormatVersion(lines);
  if (result.formatVersion > CURRENT_FORMAT_VERSION) {
    result.errors.push(`FormatVersion ${result.formatVersion} is newer than supported ${CURRENT_FORMAT_VERSION}.`);
  }

  let headerSeen = false;
  for (const raw of lines) {
    if (!raw || raw.startsWith(";") || raw.startsWith("#")) continue;
    if (raw === PRESET_HEADER) {
      headerSeen = true;
      continue;
    }
    if (!headerSeen) continue;
    if (raw.startsWith("[") && raw.endsWith("]")) {
      const section = raw.slice(1, -1);
      const normalized = section.startsWith("Region.") ? section.split(".").slice(-1)[0] : section;
      if (!ALLOWED_SECTIONS.has(normalized) && !/^Region\.[A-Za-z0-9_ -]+$/.test(section)) {
        result.errors.push(`Unknown section: ${section}`);
      }
      continue;
    }
    const eq = raw.indexOf("=");
    if (eq < 0) {
      result.errors.push(`Invalid line: ${raw.slice(0, 60)}`);
      continue;
    }
    const key = raw.slice(0, eq).trim();
    const value = raw.slice(eq + 1).trim();
    if (!ALLOWED_KEYS.has(key)) result.errors.push(`Unknown key: ${key}`);
    if (/https?:\/\//i.test(value) || /(^|[\\\/])\.\.([\\\/]|$)/.test(value) || /^[a-z]:[\\\/]/i.test(value) || /^\\\\/.test(value)) {
      result.errors.push(`Unsafe value for ${key}.`);
    }
    if (!STRING_KEYS.has(key) && key !== "Enabled") {
      const lowered = value.toLowerCase();
      const isBool = ["0", "1", "true", "false", "yes", "no", "on", "off"].includes(lowered);
      if (!isBool && value !== "") {
        const parsed = Number(value);
        if (!Number.isFinite(parsed)) result.errors.push(`Non-finite numeric value for ${key}.`);
      }
    }
  }
  result.ok = result.errors.length === 0;
  return result;
}

function cookieValue(request, name) {
  const cookie = request.headers.get("cookie") || "";
  for (const part of cookie.split(";")) {
    const [rawKey, ...rawValue] = part.trim().split("=");
    if (rawKey === name) return rawValue.join("=");
  }
  return "";
}

function base64UrlEncode(value) {
  return btoa(value).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function base64UrlDecode(value) {
  const padded = value.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((value.length + 3) % 4);
  return atob(padded);
}

async function makeAdminSession(env) {
  const payload = base64UrlEncode(JSON.stringify({ exp: Date.now() + 24 * 60 * 60 * 1000 }));
  const sig = await hmacHex(env.ADMIN_TOKEN, payload);
  return `${payload}.${sig}`;
}

async function hasValidAdminSession(request, env) {
  if (!env.ADMIN_TOKEN) return false;
  const session = cookieValue(request, "cw_admin_session");
  const [payload, sig] = session.split(".");
  if (!payload || !sig) return false;
  const expected = await hmacHex(env.ADMIN_TOKEN, payload);
  if (sig !== expected) return false;
  try {
    const data = JSON.parse(base64UrlDecode(payload));
    return Number(data.exp || 0) > Date.now();
  } catch {
    return false;
  }
}

async function isAdmin(request, env) {
  const auth = request.headers.get("authorization") || "";
  const token = env.ADMIN_TOKEN || "";
  if (token && auth === `Bearer ${token}`) return true;
  if (request.headers.get("cf-access-authenticated-user-email")) return true;
  return hasValidAdminSession(request, env);
}

function hasAdminLoginKey(request, env, body = undefined) {
  const expected = String(env.ADMIN_LOGIN_KEY || "");
  if (!expected) return true;
  const url = new URL(request.url);
  const supplied =
    url.searchParams.get("key") ||
    request.headers.get("x-cw-admin-login-key") ||
    (body && body.loginKey) ||
    "";
  return String(supplied) === expected;
}

function adminIdentity(request, env) {
  if (env.ADMIN_TOKEN && (request.headers.get("authorization") || "") === `Bearer ${env.ADMIN_TOKEN}`) return "admin-token";
  return request.headers.get("cf-access-authenticated-user-email") || "admin-session";
}

async function readJson(request) {
  try {
    const length = Number(request.headers.get("content-length") || 0);
    if (length > 140000) return null;
    return await request.json();
  } catch {
    return null;
  }
}

async function queryApprovedPresets(env) {
  const { results } = await env.DB.prepare(
    "SELECT id,title,author_name,description,tags_json,sha256,size_bytes,format_version,min_addon_version,downloads,likes,created_at,updated_at,approved_at FROM presets WHERE status='approved' AND update_of='' AND deleted_at IS NULL ORDER BY updated_at DESC"
  ).all();
  return results || [];
}

function catalogFromRows(rows, generatedAt = nowIso()) {
  return {
    schemaVersion: 1,
    generatedAt,
    presets: rows.map((row) => ({
      id: row.id,
      title: row.title,
      author: row.author_name,
      description: row.description || "",
      tags: JSON.parse(row.tags_json || "[]"),
      formatVersion: row.format_version,
      minAddonVersion: row.min_addon_version,
      publishedAt: row.approved_at || row.created_at,
      updatedAt: row.updated_at,
      downloads: row.downloads || 0,
      likes: row.likes || 0,
      file: {
        url: `/api/v1/presets/${row.id}/download`,
        sha256: row.sha256,
        size: row.size_bytes
      },
      safety: {
        approved: true
      }
    }))
  };
}

async function rebuildCatalog(env) {
  const catalog = catalogFromRows(await queryApprovedPresets(env));
  await env.PRESETS.put(CATALOG_KEY, JSON.stringify(catalog, null, 2), {
    httpMetadata: { contentType: "application/json; charset=utf-8" }
  });
  return catalog;
}

async function getCatalog(env) {
  const cached = await env.PRESETS.get(CATALOG_KEY);
  if (cached) {
    return new Response(cached.body, {
      headers: {
        "content-type": "application/json; charset=utf-8",
        "cache-control": `public, max-age=${Number(env.CATALOG_CACHE_SECONDS || 300)}`
      }
    });
  }
  return json(await rebuildCatalog(env), 200, { "cache-control": "public, max-age=60" });
}

async function submitPreset(request, env) {
  const body = await readJson(request);
  if (!body) return bad("Invalid JSON.");
  const clientId = request.headers.get("x-cw-client-id") || "";
  if (!clientId || clientId.length > 128) return bad("Missing anonymous client id.", 400);
  const title = sanitizeText(body.title, 80);
  const author = sanitizeText(body.authorName, 40) || "Anonymous";
  const description = sanitizeText(body.description, 500);
  const tags = [];
  const iniText = String(body.iniText || "");
  const maxBytes = Number(env.MAX_PRESET_BYTES || 65536);
  if (!title) return bad("Title is required.");
  const scan = scanPresetIni(iniText, maxBytes);
  if (!scan.ok) return bad("Preset validation failed.", 422, scan);
  const bytes = bytesOf(iniText);
  const hash = await sha256Hex(bytes);
  const id = `${slugify(title)}-${crypto.randomUUID().slice(0, 8)}`;
  const r2Key = `pending/${id}/preset.ini`;
  const created = nowIso();
  const submitterHash = await hashClientId(env, clientId);
  const trusted = await whitelistRow(env, submitterHash);
  const status = trusted ? "approved" : "pending";
  const r2KeyFinal = `${status}/${id}/preset.ini`;
  await env.PRESETS.put(r2Key, bytes, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
  if (trusted) {
    await env.PRESETS.put(r2KeyFinal, bytes, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
  }
  await env.DB.prepare(
    "INSERT INTO presets (id,title,author_name,description,tags_json,status,r2_key,sha256,size_bytes,format_version,min_addon_version,submitter_hash,safety_status,safety_summary,created_at,updated_at,approved_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
  ).bind(
    id, title, author, description, JSON.stringify(tags), status, trusted ? r2KeyFinal : r2Key, hash, bytes.byteLength,
    scan.formatVersion || 0, body.clientVersion || env.MIN_ADDON_VERSION || "0.6.3",
    submitterHash, "passed", trusted ? `Auto-approved whitelist ${clientFingerprint(submitterHash)}` : scan.warnings.join("; "), created, created,
    trusted ? created : null
  ).run();
  if (trusted) await rebuildCatalog(env);
  return json({ ok: true, id, status, autoApproved: Boolean(trusted), scan });
}

async function downloadPreset(request, env, id) {
  const row = await env.DB.prepare("SELECT id,r2_key,status FROM presets WHERE id=? AND status='approved' AND deleted_at IS NULL").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const object = await env.PRESETS.get(row.r2_key);
  if (!object) return bad("Preset file missing.", 404);
  const clientId = request.headers.get("x-cw-client-id") || "";
  if (clientId) {
    const deviceHash = await hashClientId(env, clientId);
    const day = new Date().toISOString().slice(0, 10);
    const existing = await env.DB.prepare(
      "SELECT 1 AS found FROM preset_downloads_daily WHERE preset_id=? AND device_hash=? AND day=?"
    ).bind(id, deviceHash, day).first();
    if (!existing) {
      await env.DB.prepare("INSERT INTO preset_downloads_daily (preset_id,device_hash,day,created_at) VALUES (?,?,?,?)")
        .bind(id, deviceHash, day, nowIso()).run();
      await env.DB.prepare("UPDATE presets SET downloads=downloads+1, updated_at=? WHERE id=?").bind(nowIso(), id).run();
      await rebuildCatalog(env);
    }
  }
  return new Response(object.body, {
    headers: {
      "content-type": "text/plain; charset=utf-8",
      "content-disposition": `attachment; filename="${id}.ini"`
    }
  });
}

async function toggleLike(request, env, id) {
  const row = await env.DB.prepare("SELECT id FROM presets WHERE id=? AND status='approved' AND deleted_at IS NULL").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const clientId = request.headers.get("x-cw-client-id") || "";
  if (!clientId) return bad("Missing anonymous client id.", 400);
  const deviceHash = await hashClientId(env, clientId);
  const existing = await env.DB.prepare("SELECT 1 FROM preset_likes WHERE preset_id=? AND device_hash=?").bind(id, deviceHash).first();
  let liked = false;
  if (existing) {
    await env.DB.prepare("DELETE FROM preset_likes WHERE preset_id=? AND device_hash=?").bind(id, deviceHash).run();
    await env.DB.prepare("UPDATE presets SET likes=MAX(0, likes-1), updated_at=? WHERE id=?").bind(nowIso(), id).run();
  } else {
    await env.DB.prepare("INSERT INTO preset_likes (preset_id,device_hash,created_at) VALUES (?,?,?)").bind(id, deviceHash, nowIso()).run();
    await env.DB.prepare("UPDATE presets SET likes=likes+1, updated_at=? WHERE id=?").bind(nowIso(), id).run();
    liked = true;
  }
  await rebuildCatalog(env);
  const updated = await env.DB.prepare("SELECT likes FROM presets WHERE id=?").bind(id).first();
  return json({ ok: true, liked, likes: updated?.likes || 0 });
}

async function listSubmissions(env, status) {
  return listAdminPresets(env, new URLSearchParams({ status: status || "pending" }), "submissions");
}

function newestPendingUpdateFilter(tableName = "presets") {
  return `NOT (
    ${tableName}.update_of<>'' AND EXISTS (
      SELECT 1 FROM presets newer
      WHERE newer.update_of=${tableName}.update_of
        AND newer.status='pending'
        AND newer.deleted_at IS NULL
        AND (
          newer.created_at > ${tableName}.created_at
          OR (newer.created_at = ${tableName}.created_at AND newer.id > ${tableName}.id)
        )
    )
  )`;
}

function adminPresetWhere(searchParams, options = {}) {
  const where = [];
  const params = [];
  const deleted = searchParams.get("deleted") || options.deleted || "active";
  if (deleted === "trash" || deleted === "1" || deleted === "true") {
    where.push("deleted_at IS NOT NULL");
  } else if (deleted !== "all") {
    where.push("deleted_at IS NULL");
  }

  const status = searchParams.get("status") || options.status || "";
  if (["pending", "approved", "rejected"].includes(status)) {
    where.push("status=?");
    params.push(status);
    if (status === "pending") {
      where.push(newestPendingUpdateFilter("presets"));
    }
  }

  const rootOnly = searchParams.get("rootOnly") || options.rootOnly || "";
  if (rootOnly === "1" || rootOnly === "true") {
    where.push("update_of=''");
  }

  const client = sanitizeText(searchParams.get("client") || "", 80).toLowerCase();
  if (/^[a-f0-9]{12,64}$/.test(client)) {
    where.push("submitter_hash LIKE ?");
    params.push(`${client}%`);
  }

  const search = likeTerm(searchParams.get("q") || "");
  if (search) {
    where.push("(id LIKE ? OR title LIKE ? OR author_name LIKE ? OR description LIKE ? OR submitter_hash LIKE ?)");
    params.push(search, search, search, search, search);
  }

  return { where: where.length ? `WHERE ${where.join(" AND ")}` : "", params };
}

function adminPresetOrder(sort) {
  switch (sort) {
    case "created": return "created_at DESC";
    case "oldest": return "created_at ASC";
    case "downloads": return "downloads DESC, updated_at DESC";
    case "likes": return "likes DESC, updated_at DESC";
    case "title": return "title COLLATE NOCASE ASC";
    case "delete_after": return "delete_after ASC, updated_at DESC";
    default: return "updated_at DESC";
  }
}

async function listAdminPresets(env, searchParams, responseKey = "presets") {
  const { where, params } = adminPresetWhere(searchParams);
  const limit = normalizeLimit(searchParams.get("limit"), 200, 500);
  const order = adminPresetOrder(searchParams.get("sort") || "");
  const { results } = await env.DB.prepare(
    `SELECT * FROM presets ${where} ORDER BY ${order} LIMIT ?`
  ).bind(...params, limit).all();
  const rows = (results || []).map(withClientFingerprint);
  return json({ ok: true, [responseKey]: rows });
}

async function audit(env, request, action, id, note = "") {
  await env.DB.prepare("INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) VALUES (?,?,?,?,?)")
    .bind(action, id || "", adminIdentity(request, env), note, nowIso()).run();
}

async function systemAudit(env, action, id, note = "") {
  await env.DB.prepare("INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) VALUES (?,?,?,?,?)")
    .bind(action, id || "", "system", note, nowIso()).run();
}

async function scalarCount(env, sql, ...params) {
  const row = await env.DB.prepare(sql).bind(...params).first();
  return Number(row?.count || 0);
}

async function adminOverview(env) {
  const [
    pending,
    approved,
    rejected,
    deleted,
    clients,
    totals,
    topPresets,
    newestPending
  ] = await Promise.all([
    scalarCount(env, "SELECT COUNT(*) AS count FROM presets WHERE status='pending' AND deleted_at IS NULL"),
    scalarCount(env, "SELECT COUNT(*) AS count FROM presets WHERE status='approved' AND update_of='' AND deleted_at IS NULL"),
    scalarCount(env, "SELECT COUNT(*) AS count FROM presets WHERE status='rejected' AND deleted_at IS NULL"),
    scalarCount(env, "SELECT COUNT(*) AS count FROM presets WHERE deleted_at IS NOT NULL"),
    scalarCount(env, "SELECT COUNT(DISTINCT submitter_hash) AS count FROM presets WHERE submitter_hash<>''"),
    env.DB.prepare("SELECT COALESCE(SUM(downloads),0) AS downloads, COALESCE(SUM(likes),0) AS likes, COUNT(*) AS presets FROM presets WHERE deleted_at IS NULL").first(),
    env.DB.prepare("SELECT id,title,author_name,downloads,likes,status,updated_at FROM presets WHERE update_of='' AND deleted_at IS NULL ORDER BY downloads DESC, likes DESC, updated_at DESC LIMIT 8").all(),
    env.DB.prepare(`SELECT id,title,author_name,created_at,updated_at,submitter_hash,update_of FROM presets WHERE status='pending' AND deleted_at IS NULL AND ${newestPendingUpdateFilter("presets")} ORDER BY created_at ASC LIMIT 8`).all()
  ]);
  return json({
    ok: true,
    counts: {
      pending,
      approved,
      rejected,
      deleted,
      clients,
      presets: Number(totals?.presets || 0),
      downloads: Number(totals?.downloads || 0),
      likes: Number(totals?.likes || 0)
    },
    topPresets: topPresets.results || [],
    newestPending: (newestPending.results || []).map(withClientFingerprint)
  });
}

async function adminGetUpdateSettings(env) {
  return json({ ok: true, update: await getUpdateSettings(env) }, 200, { "cache-control": "no-store" });
}

async function adminSaveUpdateSettings(request, env) {
  const body = await readJson(request);
  if (!body) return bad("Invalid JSON.");
  const latestVersion = normalizeVersion(body.latestVersion || body.version || "");
  if (!/^\d+(?:\.\d+){1,3}$/.test(latestVersion)) return bad("Version must look like 0.6.6.");
  const downloadPageUrl = String(body.downloadPageUrl || "").trim();
  if (!/^https:\/\/[^\s]+$/i.test(downloadPageUrl) || downloadPageUrl.length > 300) return bad("Download URL must be an https URL.");
  const changelog = String(body.changelog || "")
    .replace(/\r\n/g, "\n")
    .replace(/\r/g, "\n")
    .replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, "");
  if (!changelog.trim()) return bad("Changelog is required.");
  if (changelog.length > 50000) return bad("Changelog is too long.");
  const publishedAt = String(body.publishedAt || "").trim().slice(0, 80);
  const critical = body.critical ? "1" : "0";
  const current = await getUpdateSettings(env);
  const versionChanged = current.latestVersion !== latestVersion;
  await setAppSetting(env, request, "update.latestVersion", latestVersion);
  await setAppSetting(env, request, "update.downloadPageUrl", downloadPageUrl);
  await setAppSetting(env, request, "update.changelog", changelog);
  await setAppSetting(env, request, "update.publishedAt", publishedAt);
  await setAppSetting(env, request, "update.critical", critical);
  if (versionChanged) {
    await setAppSetting(env, request, "update.addonR2Key", "");
    await setAppSetting(env, request, "update.addonSha256", "");
    await setAppSetting(env, request, "update.addonSizeBytes", "0");
  }
  await audit(env, request, "update-settings", "", `latest=${latestVersion}`);
  return adminGetUpdateSettings(env);
}

async function adminUploadUpdateArtifact(request, env) {
  const url = new URL(request.url);
  const version = normalizeVersion(url.searchParams.get("version") || request.headers.get("x-cw-update-version") || "");
  if (!/^\d+(?:\.\d+){1,3}$/.test(version)) return bad("Version must look like 0.6.6.");
  const contentLength = Number(request.headers.get("content-length") || 0);
  if (contentLength > UPDATE_ARTIFACT_MAX_BYTES) return bad("Addon file is too large.");
  const bytes = new Uint8Array(await request.arrayBuffer());
  if (!bytes.byteLength) return bad("Addon file is empty.");
  if (bytes.byteLength > UPDATE_ARTIFACT_MAX_BYTES) return bad("Addon file is too large.");

  const sha256 = await sha256Hex(bytes);
  const r2Key = `updates/${version}/CrimsonWeather.addon64`;
  await env.PRESETS.put(r2Key, bytes, {
    httpMetadata: { contentType: "application/octet-stream" }
  });
  await setAppSetting(env, request, "update.latestVersion", version);
  await setAppSetting(env, request, "update.addonR2Key", r2Key);
  await setAppSetting(env, request, "update.addonSha256", sha256);
  await setAppSetting(env, request, "update.addonSizeBytes", String(bytes.byteLength));
  await audit(env, request, "update-artifact", "", `version=${version} sha256=${sha256}`);
  return json({
    ok: true,
    version,
    addonR2Key: r2Key,
    addonSha256: sha256,
    addonSizeBytes: bytes.byteLength
  }, 200, { "cache-control": "no-store" });
}

async function listAdminClients(env, searchParams) {
  const where = ["p.submitter_hash<>''"];
  const params = [];
  const search = likeTerm(searchParams.get("q") || "");
  if (search) {
    where.push("(p.submitter_hash LIKE ? OR p.title LIKE ? OR p.author_name LIKE ? OR w.label LIKE ? OR w.note LIKE ?)");
    params.push(search, search, search, search, search);
  }
  const orderName = searchParams.get("sort") || "";
  const order = orderName === "uploads" ? "upload_count DESC, last_upload DESC"
    : orderName === "downloads" ? "total_downloads DESC, last_upload DESC"
    : orderName === "likes" ? "total_likes DESC, last_upload DESC"
    : "last_upload DESC";
  const limit = normalizeLimit(searchParams.get("limit"), 200, 500);
  const { results } = await env.DB.prepare(
    `SELECT
       p.submitter_hash,
       COUNT(*) AS upload_count,
       SUM(CASE WHEN p.status='pending' AND p.deleted_at IS NULL THEN 1 ELSE 0 END) AS pending_count,
       SUM(CASE WHEN p.status='approved' AND p.update_of='' AND p.deleted_at IS NULL THEN 1 ELSE 0 END) AS approved_count,
       SUM(CASE WHEN p.status='rejected' AND p.deleted_at IS NULL THEN 1 ELSE 0 END) AS rejected_count,
       SUM(CASE WHEN p.deleted_at IS NOT NULL THEN 1 ELSE 0 END) AS deleted_count,
       COALESCE(SUM(p.downloads),0) AS total_downloads,
       COALESCE(SUM(p.likes),0) AS total_likes,
       MIN(p.created_at) AS first_upload,
       MAX(p.updated_at) AS last_upload,
       COALESCE(w.label,'') AS label,
       COALESCE(w.note,'') AS note,
       COALESCE(w.auto_approve,0) AS auto_approve,
       w.updated_at AS whitelist_updated_at
     FROM presets p
     LEFT JOIN client_whitelist w ON w.submitter_hash=p.submitter_hash
     WHERE ${where.join(" AND ")}
     GROUP BY p.submitter_hash
     ORDER BY ${order}
     LIMIT ?`
  ).bind(...params, limit).all();
  return json({ ok: true, clients: (results || []).map(withClientFingerprint) });
}

async function listAdminAudit(env, searchParams) {
  const limit = normalizeLimit(searchParams.get("limit"), 100, 300);
  const { results } = await env.DB.prepare("SELECT * FROM admin_audit ORDER BY created_at DESC LIMIT ?").bind(limit).all();
  return json({ ok: true, audit: results || [] });
}

async function approveSubmission(request, env, id) {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND status='pending' AND deleted_at IS NULL").bind(id).first();
  if (!row) return bad("Pending submission not found.", 404);
  const object = await env.PRESETS.get(row.r2_key);
  if (!object) return bad("Pending preset file missing.", 404);
  if (row.update_of) {
    const newer = await env.DB.prepare(
      `SELECT id FROM presets
       WHERE update_of=? AND status='pending' AND deleted_at IS NULL
         AND (created_at > ? OR (created_at = ? AND id > ?))
       ORDER BY created_at DESC, id DESC LIMIT 1`
    ).bind(row.update_of, row.created_at || "", row.created_at || "", row.id).first();
    if (newer) return bad(`A newer pending update exists for this preset: ${newer.id}`, 409);
    const target = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND update_of='' AND deleted_at IS NULL").bind(row.update_of).first();
    if (!target) return bad("Target preset for update was not found.", 404);
    const approvedKey = `approved/${target.id}/preset.ini`;
    const historyKey = `history/${target.id}/${id}/preset.ini`;
    const bytes = await object.arrayBuffer();
    await env.PRESETS.put(approvedKey, bytes, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
    await env.PRESETS.put(historyKey, bytes, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
    const approvedAt = target.approved_at || nowIso();
    const reviewedAt = nowIso();
    const retainUntil = addDaysIso(7);
    await env.DB.prepare(
      "UPDATE presets SET title=?,author_name=?,description=?,tags_json=?,status='approved',r2_key=?,sha256=?,size_bytes=?,format_version=?,min_addon_version=?,safety_status=?,safety_summary=?,updated_at=?,approved_at=? WHERE id=?"
    ).bind(
      row.title, row.author_name, row.description, row.tags_json, approvedKey, row.sha256, row.size_bytes,
      row.format_version, row.min_addon_version, row.safety_status, row.safety_summary, reviewedAt, approvedAt, target.id
    ).run();
    await env.DB.prepare(
      "UPDATE presets SET status='approved',r2_key=?,approved_at=?,updated_at=?,deleted_at=?,delete_after=?,deleted_by=?,delete_reason=? WHERE id=?"
    ).bind(historyKey, reviewedAt, reviewedAt, reviewedAt, retainUntil, adminIdentity(request, env), `applied to ${target.id}`, id).run();
    if (row.r2_key && row.r2_key !== historyKey) await env.PRESETS.delete(row.r2_key);
    await audit(env, request, "approve-update", target.id, id);
    await rebuildCatalog(env);
    return json({ ok: true, id: target.id, updateId: id, status: "approved", retainedUntil: retainUntil });
  }
  const approvedKey = `approved/${id}/preset.ini`;
  await env.PRESETS.put(approvedKey, object.body, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
  await env.DB.prepare("UPDATE presets SET status='approved', r2_key=?, approved_at=?, updated_at=? WHERE id=?")
    .bind(approvedKey, nowIso(), nowIso(), id).run();
  await audit(env, request, "approve", id);
  await rebuildCatalog(env);
  return json({ ok: true, id, status: "approved" });
}

async function rejectSubmission(request, env, id) {
  const body = await readJson(request) || {};
  const row = await env.DB.prepare("SELECT id FROM presets WHERE id=? AND status='pending' AND deleted_at IS NULL").bind(id).first();
  if (!row) return bad("Pending submission not found.", 404);
  await env.DB.prepare("UPDATE presets SET status='rejected', rejected_at=?, updated_at=? WHERE id=?").bind(nowIso(), nowIso(), id).run();
  await audit(env, request, "reject", id, sanitizeText(body.note, 300));
  return json({ ok: true, id, status: "rejected" });
}

async function softDeletePreset(request, env, id, reason = "") {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND deleted_at IS NULL").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const deletedAt = nowIso();
  const deleteAfter = addDaysIso(7);
  const note = sanitizeText(reason || row.status || "", 300);
  await env.DB.prepare(
    "UPDATE presets SET deleted_at=?, delete_after=?, deleted_by=?, delete_reason=?, updated_at=? WHERE id=?"
  ).bind(deletedAt, deleteAfter, adminIdentity(request, env), note, deletedAt, id).run();
  await audit(env, request, "soft-delete", id, note);
  if (row.status === "approved" && !row.update_of) await rebuildCatalog(env);
  return json({ ok: true, id, deleted: true, deletedAt, deleteAfter });
}

async function restorePresetAdmin(request, env, id) {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND deleted_at IS NOT NULL").bind(id).first();
  if (!row) return bad("Deleted preset not found.", 404);
  await env.DB.prepare(
    "UPDATE presets SET deleted_at=NULL, delete_after=NULL, deleted_by='', delete_reason='', updated_at=? WHERE id=?"
  ).bind(nowIso(), id).run();
  await audit(env, request, "restore", id, row.delete_reason || "");
  if (row.status === "approved" && !row.update_of) await rebuildCatalog(env);
  return json({ ok: true, id, restored: true });
}

async function deletePresetFiles(env, id, row) {
  const keys = new Set([
    row?.r2_key,
    `pending/${id}/preset.ini`,
    `approved/${id}/preset.ini`
  ]);
  for (const key of keys) {
    if (key) await env.PRESETS.delete(key);
  }
}

async function purgePresetAdmin(request, env, id, action = "purge") {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=?").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  await deletePresetFiles(env, id, row);
  await env.DB.prepare("DELETE FROM preset_likes WHERE preset_id=?").bind(id).run();
  await env.DB.prepare("DELETE FROM preset_downloads_daily WHERE preset_id=?").bind(id).run();
  await env.DB.prepare("DELETE FROM presets WHERE id=?").bind(id).run();
  await audit(env, request, action, id, row.status || "");
  if (row.status === "approved" && !row.update_of) await rebuildCatalog(env);
  return json({ ok: true, id, purged: true });
}

async function purgeExpiredDeletedPresets(env) {
  const now = nowIso();
  const { results } = await env.DB.prepare(
    "SELECT * FROM presets WHERE deleted_at IS NOT NULL AND delete_after IS NOT NULL AND delete_after<=? ORDER BY delete_after ASC LIMIT 100"
  ).bind(now).all();
  const rows = results || [];
  let purged = 0;
  let rebuilt = false;
  for (const row of rows) {
    await deletePresetFiles(env, row.id, row);
    await env.DB.prepare("DELETE FROM preset_likes WHERE preset_id=?").bind(row.id).run();
    await env.DB.prepare("DELETE FROM preset_downloads_daily WHERE preset_id=?").bind(row.id).run();
    await env.DB.prepare("DELETE FROM presets WHERE id=?").bind(row.id).run();
    await env.DB.prepare("INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) VALUES (?,?,?,?,?)")
      .bind("auto-purge", row.id, "scheduled-worker", row.status || "", nowIso()).run();
    if (row.status === "approved" && !row.update_of) rebuilt = true;
    purged += 1;
  }
  if (rebuilt) await rebuildCatalog(env);
  return { ok: true, purged };
}

async function whitelistSubmitterFromPreset(env, request, id, label = "", note = "") {
  const row = await env.DB.prepare("SELECT id,title,author_name,submitter_hash FROM presets WHERE id=?").bind(id).first();
  if (!row || !row.submitter_hash) return bad("Preset submitter was not found.", 404);
  const now = nowIso();
  const finalLabel = sanitizeText(label, 80) || row.author_name || row.title || id;
  const finalNote = sanitizeText(note, 300);
  await env.DB.prepare(
    "INSERT INTO client_whitelist (submitter_hash,label,auto_approve,note,created_at,updated_at) VALUES (?,?,?,?,?,?) ON CONFLICT(submitter_hash) DO UPDATE SET label=excluded.label,auto_approve=excluded.auto_approve,note=excluded.note,updated_at=excluded.updated_at"
  ).bind(row.submitter_hash, finalLabel, 1, finalNote, now, now).run();
  await audit(env, request, "whitelist-from-preset", id, clientFingerprint(row.submitter_hash));
  return json({ ok: true, submitterHash: row.submitter_hash, fingerprint: clientFingerprint(row.submitter_hash), label: finalLabel });
}

async function batchSubmissions(request, env) {
  const body = await readJson(request);
  const approvals = Array.isArray(body?.approve) ? body.approve : [];
  const rejections = Array.isArray(body?.reject) ? body.reject : [];
  const deletions = Array.isArray(body?.delete) ? body.delete : [];
  const restores = Array.isArray(body?.restore) ? body.restore : [];
  const purges = Array.isArray(body?.purge) ? body.purge : [];
  const whitelists = Array.isArray(body?.whitelist) ? body.whitelist : [];
  const results = [];
  for (const id of approvals) results.push(await (await approveSubmission(request, env, String(id))).json());
  for (const id of rejections) results.push(await (await rejectSubmission(request, env, String(id))).json());
  for (const id of deletions) results.push(await (await softDeletePreset(request, env, String(id), "batch")).json());
  for (const id of restores) results.push(await (await restorePresetAdmin(request, env, String(id))).json());
  for (const id of purges) results.push(await (await purgePresetAdmin(request, env, String(id), "batch-purge")).json());
  for (const id of whitelists) results.push(await (await whitelistSubmitterFromPreset(env, request, String(id))).json());
  return json({ ok: true, results });
}

async function adminPresetDetail(env, id) {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=?").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const object = await env.PRESETS.get(row.r2_key);
  const iniText = object ? await object.text() : "";
  return json({
    ok: true,
    preset: withClientFingerprint(row),
    iniText,
    scan: iniText ? scanPresetIni(iniText) : { ok: false, errors: ["Preset file missing."], warnings: [] }
  });
}

async function adminPresetIni(env, id) {
  const row = await env.DB.prepare("SELECT id,r2_key FROM presets WHERE id=?").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const object = await env.PRESETS.get(row.r2_key);
  if (!object) return bad("Preset file missing.", 404);
  return new Response(object.body, {
    headers: {
      "content-type": "text/plain; charset=utf-8",
      "content-disposition": `attachment; filename="${id}.ini"`
    }
  });
}

async function deletePresetAdmin(request, env, id) {
  const body = await readJson(request) || {};
  return softDeletePreset(request, env, id, sanitizeText(body.reason || "admin", 300));
}

async function listMyPresets(request, env) {
  const submitterHash = await submitterHashFromRequest(request, env);
  if (!submitterHash) return bad("Missing anonymous client id.", 400);
  const { results } = await env.DB.prepare(
    `SELECT
       p.id,p.title,p.author_name,p.description,p.tags_json,p.status,p.update_of,p.downloads,p.likes,p.created_at,p.updated_at,p.approved_at,p.rejected_at,
       u.id AS pending_update_id,
       u.title AS pending_update_title,
       u.updated_at AS pending_update_at
     FROM presets p
     LEFT JOIN presets u ON u.update_of=p.id AND u.status='pending' AND u.deleted_at IS NULL
       AND NOT EXISTS (
         SELECT 1 FROM presets newer
         WHERE newer.update_of=p.id
           AND newer.status='pending'
           AND newer.deleted_at IS NULL
           AND (
             newer.created_at > u.created_at
             OR (newer.created_at = u.created_at AND newer.id > u.id)
           )
       )
     WHERE p.submitter_hash=? AND p.update_of='' AND p.deleted_at IS NULL
     ORDER BY p.updated_at DESC LIMIT 100`
  ).bind(submitterHash).all();
  return json({ ok: true, clientFingerprint: clientFingerprint(submitterHash), presets: results || [] });
}

async function deleteMyPreset(request, env, id) {
  const submitterHash = await submitterHashFromRequest(request, env);
  if (!submitterHash) return bad("Missing anonymous client id.", 400);
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND submitter_hash=? AND deleted_at IS NULL").bind(id, submitterHash).first();
  if (!row) return bad("Preset not found for this client.", 404);
  const deletedAt = nowIso();
  await env.DB.prepare(
    "UPDATE presets SET deleted_at=?, delete_after=?, deleted_by=?, delete_reason=?, updated_at=? WHERE id=?"
  ).bind(deletedAt, addDaysIso(7), `client:${clientFingerprint(submitterHash)}`, "client delete", deletedAt, id).run();
  if (row.status === "approved" && !row.update_of) await rebuildCatalog(env);
  return json({ ok: true, id, deleted: true });
}

async function cancelMyPresetUpdate(request, env, id) {
  const submitterHash = await submitterHashFromRequest(request, env);
  if (!submitterHash) return bad("Missing anonymous client id.", 400);
  const target = await env.DB.prepare("SELECT id FROM presets WHERE id=? AND submitter_hash=? AND update_of='' AND deleted_at IS NULL").bind(id, submitterHash).first();
  if (!target) return bad("Preset not found for this client.", 404);
  const row = await env.DB.prepare(
    `SELECT * FROM presets
     WHERE update_of=? AND submitter_hash=? AND status='pending' AND deleted_at IS NULL
     ORDER BY created_at DESC, id DESC LIMIT 1`
  ).bind(id, submitterHash).first();
  if (!row) return bad("Pending update not found for this preset.", 404);
  const cancelledAt = nowIso();
  const retainUntil = addDaysIso(7);
  await env.DB.prepare(
    "UPDATE presets SET status='rejected',rejected_at=?,deleted_at=?,delete_after=?,deleted_by=?,delete_reason=?,updated_at=? WHERE id=?"
  ).bind(cancelledAt, cancelledAt, retainUntil, `client:${clientFingerprint(submitterHash)}`, "client cancelled update", cancelledAt, row.id).run();
  await systemAudit(env, "client-cancel-update", row.id, `target=${id}`);
  return json({ ok: true, id, updateId: row.id, cancelled: true, retainedUntil: retainUntil });
}

async function updateMyPreset(request, env, id) {
  const submitterHash = await submitterHashFromRequest(request, env);
  if (!submitterHash) return bad("Missing anonymous client id.", 400);
  const target = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND submitter_hash=? AND update_of='' AND deleted_at IS NULL").bind(id, submitterHash).first();
  if (!target) return bad("Preset not found for this client.", 404);
  const body = await readJson(request);
  if (!body) return bad("Invalid JSON.");
  const title = sanitizeText(body.title, 80);
  const author = sanitizeText(body.authorName, 40) || "Anonymous";
  const description = sanitizeText(body.description, 500);
  const tags = [];
  const iniText = String(body.iniText || "");
  const maxBytes = Number(env.MAX_PRESET_BYTES || 65536);
  if (!title) return bad("Title is required.");
  const scan = scanPresetIni(iniText, maxBytes);
  if (!scan.ok) return bad("Preset validation failed.", 422, scan);
  const bytes = bytesOf(iniText);
  const hash = await sha256Hex(bytes);
  const trusted = await whitelistRow(env, submitterHash);
  const updated = nowIso();

  if (target.status !== "approved" || trusted) {
    const r2Key = target.status === "approved" ? `approved/${id}/preset.ini` : target.r2_key;
    await env.PRESETS.put(r2Key, bytes, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
    await env.DB.prepare(
      "UPDATE presets SET title=?,author_name=?,description=?,tags_json=?,r2_key=?,sha256=?,size_bytes=?,format_version=?,min_addon_version=?,safety_status=?,safety_summary=?,updated_at=?,approved_at=COALESCE(approved_at, ?) WHERE id=?"
    ).bind(
      title, author, description, JSON.stringify(tags), r2Key, hash, bytes.byteLength,
      scan.formatVersion || 0, body.clientVersion || env.MIN_ADDON_VERSION || "0.6.3",
      "passed", trusted ? `Auto-approved whitelist ${clientFingerprint(submitterHash)}` : scan.warnings.join("; "),
      updated, target.status === "approved" ? updated : null, id
    ).run();
    if (target.status === "approved") await rebuildCatalog(env);
    return json({ ok: true, id, status: target.status, autoApproved: Boolean(trusted), scan });
  }

  const updateId = `${slugify(title)}-update-${crypto.randomUUID().slice(0, 8)}`;
  const r2Key = `pending/${updateId}/preset.ini`;
  await env.PRESETS.put(r2Key, bytes, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
  await env.DB.prepare(
    "INSERT INTO presets (id,title,author_name,description,tags_json,status,r2_key,sha256,size_bytes,format_version,min_addon_version,submitter_hash,safety_status,safety_summary,created_at,updated_at,update_of) VALUES (?,?,?,?,?,'pending',?,?,?,?,?,?,?,?,?,?,?)"
  ).bind(
    updateId, title, author, description, JSON.stringify(tags), r2Key, hash, bytes.byteLength,
    scan.formatVersion || 0, body.clientVersion || env.MIN_ADDON_VERSION || "0.6.3",
    submitterHash, "passed", scan.warnings.join("; "), updated, updated, id
  ).run();
  await supersedeOlderPendingUpdates(env, id, updateId);
  return json({ ok: true, id, updateId, status: "pending", autoApproved: false, scan });
}

async function supersedeOlderPendingUpdates(env, targetId, keepId) {
  const { results } = await env.DB.prepare(
    "SELECT id FROM presets WHERE update_of=? AND status='pending' AND deleted_at IS NULL AND id<>?"
  ).bind(targetId, keepId || "").all();
  const rows = results || [];
  if (rows.length) {
    const supersededAt = nowIso();
    const retainUntil = addDaysIso(7);
    await env.DB.prepare(
      "UPDATE presets SET status='rejected',rejected_at=?,deleted_at=?,delete_after=?,deleted_by=?,delete_reason=?,updated_at=? WHERE update_of=? AND status='pending' AND deleted_at IS NULL AND id<>?"
    ).bind(supersededAt, supersededAt, retainUntil, "system", `superseded by ${keepId}`, supersededAt, targetId, keepId || "").run();
    for (const row of rows) {
      await systemAudit(env, "supersede-update", row.id, `replaced by ${keepId}`);
    }
  }
}

async function listWhitelist(env) {
  const { results } = await env.DB.prepare("SELECT * FROM client_whitelist ORDER BY updated_at DESC LIMIT 200").all();
  return json({ ok: true, clients: (results || []).map(withClientFingerprint) });
}

async function addWhitelist(request, env) {
  const body = await readJson(request);
  if (!body) return bad("Invalid JSON.");
  const rawClientId = sanitizeText(body.clientId, 160);
  const providedHash = String(body.submitterHash || "").trim().toLowerCase();
  let submitterHash = "";
  if (rawClientId) {
    submitterHash = await hashClientId(env, rawClientId);
  } else if (/^[a-f0-9]{64}$/.test(providedHash)) {
    submitterHash = providedHash;
  }
  if (!submitterHash) return bad("Provide a raw ClientId or a 64-character submitter hash.");
  const label = sanitizeText(body.label, 80);
  const note = sanitizeText(body.note, 300);
  const autoApprove = body.autoApprove === false ? 0 : 1;
  const now = nowIso();
  await env.DB.prepare(
    "INSERT INTO client_whitelist (submitter_hash,label,auto_approve,note,created_at,updated_at) VALUES (?,?,?,?,?,?) ON CONFLICT(submitter_hash) DO UPDATE SET label=excluded.label,auto_approve=excluded.auto_approve,note=excluded.note,updated_at=excluded.updated_at"
  ).bind(submitterHash, label, autoApprove, note, now, now).run();
  await audit(env, request, "whitelist-add", "", clientFingerprint(submitterHash));
  return json({ ok: true, submitterHash, fingerprint: clientFingerprint(submitterHash), autoApprove: Boolean(autoApprove) });
}

async function whitelistFromPreset(request, env, id) {
  const body = await readJson(request) || {};
  return whitelistSubmitterFromPreset(env, request, id, body.label || "", body.note || "");
}

async function deleteWhitelist(request, env, submitterHash) {
  if (!/^[a-f0-9]{64}$/i.test(submitterHash)) return bad("Invalid submitter hash.", 400);
  await env.DB.prepare("DELETE FROM client_whitelist WHERE submitter_hash=?").bind(submitterHash.toLowerCase()).run();
  await audit(env, request, "whitelist-delete", "", clientFingerprint(submitterHash));
  return json({ ok: true, deleted: true });
}

function crc32(bytes) {
  let c = ~0;
  for (const b of bytes) {
    c ^= b;
    for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1));
  }
  return (~c) >>> 0;
}

function u16(n) { return [n & 255, (n >>> 8) & 255]; }
function u32(n) { return [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255]; }

function zipStore(files) {
  const chunks = [];
  const central = [];
  let offset = 0;
  for (const file of files) {
    const name = bytesOf(file.name);
    const data = typeof file.data === "string" ? bytesOf(file.data) : file.data;
    const crc = crc32(data);
    const local = new Uint8Array([
      ...u32(0x04034b50), ...u16(20), ...u16(0), ...u16(0), ...u16(0), ...u16(0),
      ...u32(crc), ...u32(data.length), ...u32(data.length), ...u16(name.length), ...u16(0)
    ]);
    chunks.push(local, name, data);
    central.push({ name, crc, size: data.length, offset });
    offset += local.length + name.length + data.length;
  }
  const centralStart = offset;
  for (const entry of central) {
    const header = new Uint8Array([
      ...u32(0x02014b50), ...u16(20), ...u16(20), ...u16(0), ...u16(0), ...u16(0), ...u16(0),
      ...u32(entry.crc), ...u32(entry.size), ...u32(entry.size), ...u16(entry.name.length),
      ...u16(0), ...u16(0), ...u16(0), ...u16(0), ...u32(0), ...u32(entry.offset)
    ]);
    chunks.push(header, entry.name);
    offset += header.length + entry.name.length;
  }
  const centralSize = offset - centralStart;
  chunks.push(new Uint8Array([
    ...u32(0x06054b50), ...u16(0), ...u16(0), ...u16(central.length), ...u16(central.length),
    ...u32(centralSize), ...u32(centralStart), ...u16(0)
  ]));
  return new Blob(chunks, { type: "application/zip" });
}

async function exportPending(env) {
  const { results } = await env.DB.prepare(`SELECT * FROM presets WHERE status='pending' AND deleted_at IS NULL AND ${newestPendingUpdateFilter("presets")} ORDER BY created_at ASC`).all();
  const rows = results || [];
  const files = [{ name: "manifest.json", data: JSON.stringify({ exportedAt: nowIso(), submissions: rows }, null, 2) }];
  for (const row of rows) {
    const object = await env.PRESETS.get(row.r2_key);
    const ini = object ? await object.text() : "";
    const scan = scanPresetIni(ini);
    files.push({ name: `${row.id}/preset.ini`, data: ini });
    files.push({ name: `${row.id}/metadata.json`, data: JSON.stringify(row, null, 2) });
    files.push({ name: `${row.id}/scan.json`, data: JSON.stringify(scan, null, 2) });
  }
  return new Response(zipStore(files), {
    headers: {
      "content-type": "application/zip",
      "content-disposition": "attachment; filename=\"crimson-weather-pending-presets.zip\""
    }
  });
}

async function loginAdmin(request, env) {
  const body = await readJson(request);
  if (!hasAdminLoginKey(request, env, body)) return text("Not found.", 404);
  if (!env.ADMIN_TOKEN || !body || body.token !== env.ADMIN_TOKEN) {
    return bad("Invalid admin token.", 401);
  }
  const session = await makeAdminSession(env);
  return json({ ok: true }, 200, {
    "set-cookie": `cw_admin_session=${session}; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=86400`
  });
}

function logoutAdmin() {
  return json({ ok: true }, 200, {
    "set-cookie": "cw_admin_session=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0"
  });
}

const ADMIN_LOGIN_HTML = `<!doctype html>
<html><head><meta charset="utf-8"><title>Crimson Weather Admin Login</title>
<style>body{font-family:Segoe UI,Arial,sans-serif;margin:24px;background:#111;color:#eee}input,button{font:inherit;padding:8px;margin:4px 0}input{width:min(520px,100%);background:#0c0c0c;color:#eee;border:1px solid #333}button{display:block}.err{color:#ff9b9b}</style></head>
<body><h1>Crimson Weather Admin</h1>
<p>Paste your Worker ADMIN_TOKEN to create a local browser admin session.</p>
<input id="token" type="password" autocomplete="current-password" autofocus>
<button onclick="login()">Log In</button>
<p id="msg" class="err"></p>
<script>
const loginKey=new URLSearchParams(location.search).get('key')||'';
async function login(){const token=document.getElementById('token').value; const r=await fetch('/api/v1/admin/login',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({token,loginKey})}); if(r.ok){location.href='/admin';return;} document.getElementById('msg').textContent=await r.text();}
</script></body></html>`;

const ADMIN_HTML = `<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Crimson Weather Admin</title>
<style>
:root{color-scheme:dark;--bg:#0f1013;--panel:#181a1f;--panel2:#20232a;--line:#343842;--text:#f2f4f8;--muted:#aeb6c2;--accent:#5d7fc4;--good:#5fb980;--warn:#d3aa55;--bad:#c76565}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 Segoe UI,Arial,sans-serif}button,input,select,textarea{font:inherit}button{border:1px solid #6684bf;background:#344f82;color:#fff;padding:7px 11px;cursor:pointer}button.secondary{background:#242832;border-color:#4b5362}button.danger{background:#6b2929;border-color:#a84f4f}button.good{background:#2e6845;border-color:#58a375}button:disabled{opacity:.45;cursor:not-allowed}input,select,textarea{background:#101218;color:var(--text);border:1px solid var(--line);padding:7px 9px}main{max-width:1480px;margin:0 auto;padding:18px}.top{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:14px}.top h1{font-size:22px;margin:0}.tabs{display:flex;gap:6px;flex-wrap:wrap;margin:12px 0 16px}.tab{background:#1d2129;border-color:#3d4656}.tab.active{background:#3a568c;border-color:#6f8ccc}.toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:10px 0}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}.card,.panel{background:var(--panel);border:1px solid var(--line);padding:12px}.card b{font-size:24px;display:block}.muted{color:var(--muted);font-size:12px}.message{min-height:20px;color:#ffcf8a;margin:8px 0}.table{width:100%;border-collapse:collapse;background:var(--panel)}th,td{border-bottom:1px solid var(--line);padding:8px;text-align:left;vertical-align:top}th{background:var(--panel2);color:#dfe6ef;position:sticky;top:0}tr:hover td{background:#1b1f27}.pill{display:inline-block;border:1px solid #555f70;background:#252a34;padding:1px 6px;margin-left:5px;color:#dce4ef}.pending{border-color:var(--warn);color:#ffd991}.approved{border-color:var(--good);color:#a9e8bd}.rejected,.deleted{border-color:var(--bad);color:#ffb0b0}.drawer{position:fixed;inset:20px;background:#14161b;border:1px solid #5d6574;box-shadow:0 18px 70px #000;z-index:5;padding:16px;overflow:auto}.drawer[hidden],section[hidden]{display:none}.drawer header{display:flex;justify-content:space-between;align-items:flex-start;gap:12px}.split{display:grid;grid-template-columns:minmax(280px,420px) 1fr;gap:12px}pre{white-space:pre-wrap;overflow:auto;background:#0b0d11;border:1px solid #303541;padding:10px;max-height:520px}.right{text-align:right}.empty{padding:18px;color:var(--muted)}.nowrap{white-space:nowrap}.search{min-width:280px}.dangerText{color:#ffb0b0}.clientHash{font-family:Consolas,monospace;font-size:12px;color:#c8d4e4}
</style></head>
<body><main>
<div class="top"><h1>Crimson Weather Community Admin</h1><div><button class="secondary" data-action="rebuild">Rebuild Catalog</button> <a href="/api/v1/admin/submissions/export.zip"><button class="secondary">Download Pending ZIP</button></a> <button class="secondary" data-action="logout">Log Out</button></div></div>
<div class="tabs">
  <button class="tab active" data-tab="dashboard">Dashboard</button>
  <button class="tab" data-tab="review">Review Queue</button>
  <button class="tab" data-tab="presets">Presets</button>
  <button class="tab" data-tab="clients">Clients</button>
  <button class="tab" data-tab="update">Update</button>
  <button class="tab" data-tab="trash">Trash</button>
  <button class="tab" data-tab="audit">Audit</button>
</div>
<div id="message" class="message"></div>

<section id="dashboard">
  <div id="cards" class="grid"></div>
  <div class="split" style="margin-top:12px">
    <div class="panel"><h2>Newest Pending</h2><div id="dashPending"></div></div>
    <div class="panel"><h2>Top Presets</h2><div id="dashTop"></div></div>
  </div>
</section>

<section id="review" hidden>
  <div class="toolbar"><button class="good" data-bulk="approve">Approve Selected</button><button class="secondary" data-bulk="reject">Reject Selected</button><button class="danger" data-bulk="delete">Delete Selected</button><button class="secondary" data-action="refresh">Refresh</button></div>
  <div id="reviewTable"></div>
</section>

<section id="presets" hidden>
  <div class="toolbar"><input id="presetSearch" class="search" placeholder="Search title, author, id, client..."><select id="presetStatus"><option value="">All active</option><option value="approved">Approved</option><option value="pending">Pending</option><option value="rejected">Rejected</option></select><select id="presetSort"><option value="updated">Updated</option><option value="created">Created</option><option value="downloads">Downloads</option><option value="likes">Likes</option><option value="title">Title</option></select><button class="secondary" data-action="loadPresets">Search</button></div>
  <div id="presetTable"></div>
</section>

<section id="clients" hidden>
  <div class="toolbar"><input id="clientSearch" class="search" placeholder="Search client hash, label, author..."><select id="clientSort"><option value="last">Last upload</option><option value="uploads">Uploads</option><option value="downloads">Downloads</option><option value="likes">Likes</option></select><button class="secondary" data-action="loadClients">Search</button></div>
  <div class="panel"><h2>Whitelist Client</h2><div class="toolbar"><input id="wlClientId" class="search" placeholder="Raw ClientId or submitter hash"><input id="wlLabel" placeholder="Label"><input id="wlNote" placeholder="Note"><button data-action="addWhitelist">Whitelist</button></div></div>
  <div id="clientTable" style="margin-top:12px"></div>
</section>

<section id="update" hidden>
  <div class="panel">
    <h2>Addon Update Notice</h2>
    <p class="muted">This controls what the addon sees in the overlay header. Saving here updates /api/v1/update without changing addon code.</p>
    <div class="toolbar">
      <label>Latest Version <input id="updateVersion" placeholder="0.6.6"></label>
      <label>Published <input id="updatePublished" placeholder="2026-05-22"></label>
      <label><input id="updateCritical" type="checkbox"> Critical</label>
    </div>
    <div class="toolbar">
      <label style="flex:1">Download URL <input id="updateUrl" style="width:100%" placeholder="https://www.nexusmods.com/crimsondesert/mods/632?tab=files"></label>
    </div>
    <div class="toolbar">
      <label style="flex:1">Direct .addon64 <input id="updateArtifact" type="file" accept=".addon64,application/octet-stream" style="width:100%"></label>
      <button class="secondary" data-action="uploadUpdateArtifact">Upload .addon64</button>
    </div>
    <div id="updateArtifactInfo" class="muted"></div>
    <textarea id="updateChangelog" spellcheck="false" style="width:100%;min-height:520px;font-family:Consolas,monospace"></textarea>
    <div class="toolbar">
      <button class="good" data-action="saveUpdate">Save Update Notice</button>
      <button class="secondary" data-action="loadUpdate">Reload</button>
      <span id="updateSource" class="muted"></span>
    </div>
  </div>
</section>

<section id="trash" hidden>
  <div class="toolbar"><input id="trashSearch" class="search" placeholder="Search trash..."><button class="secondary" data-action="loadTrash">Search</button><button class="secondary" data-bulk="restore">Restore Selected</button><button class="danger" data-bulk="purge">Purge Selected</button></div>
  <div id="trashTable"></div>
</section>

<section id="audit" hidden>
  <div class="toolbar"><button class="secondary" data-action="loadAudit">Refresh</button></div>
  <div id="auditTable"></div>
</section>

<section id="detail" class="drawer" hidden>
  <header><div><h2 id="detailTitle"></h2><div id="detailMeta" class="muted"></div></div><button class="secondary" data-action="closeDetail">Close</button></header>
  <div class="split"><div><h3>Metadata</h3><pre id="detailJson"></pre><h3>Scan</h3><pre id="detailScan"></pre></div><div><h3>INI Content</h3><pre id="detailIni"></pre></div></div>
</section>
</main>
<script>
const state={tab:'dashboard',selected:new Set()};
const qs=(s)=>document.querySelector(s);
const qsa=(s)=>Array.from(document.querySelectorAll(s));
async function api(path,opts){const r=await fetch(path,opts); if(!r.ok) throw new Error(await r.text()); return r.json();}
function esc(s){return String(s??'').replace(/[&<>"]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c];});}
function msg(text){qs('#message').textContent=text||'';}
function pill(row){const cls=row.deleted_at?'deleted':row.status;return '<span class="pill '+cls+'">'+esc(row.deleted_at?'deleted':row.status)+'</span>';}
function client(row){return row.client_fingerprint?'<span class="clientHash">'+esc(row.client_fingerprint)+'</span>':'';}
function selectCell(row){return '<input type="checkbox" class="rowcheck" data-id="'+esc(row.id)+'">';}
function actionButton(action,id,label,cls){return '<button class="'+(cls||'secondary')+'" data-action="'+action+'" data-id="'+esc(id)+'">'+label+'</button>';}
function selectedIds(){return qsa('.rowcheck:checked').map(function(x){return x.dataset.id;});}
function table(headers,rows,empty){if(!rows.length)return '<div class="empty">'+esc(empty||'No rows.')+'</div>';return '<table class="table"><thead><tr>'+headers.map(function(h){return '<th>'+h+'</th>';}).join('')+'</tr></thead><tbody>'+rows.join('')+'</tbody></table>';}
async function setTab(tab){state.tab=tab;qsa('.tab').forEach(function(b){b.classList.toggle('active',b.dataset.tab===tab);});qsa('main>section').forEach(function(s){if(s.id!=='detail')s.hidden=s.id!==tab;});msg('');await loadActive();}
async function loadActive(){if(state.tab==='dashboard')return loadDashboard();if(state.tab==='review')return loadReview();if(state.tab==='presets')return loadPresets();if(state.tab==='clients')return loadClients();if(state.tab==='update')return loadUpdate();if(state.tab==='trash')return loadTrash();if(state.tab==='audit')return loadAudit();}
async function loadDashboard(){const d=await api('/api/v1/admin/overview');const cards=[['Pending',d.counts.pending],['Approved',d.counts.approved],['Rejected',d.counts.rejected],['Trash',d.counts.deleted],['Clients',d.counts.clients],['Downloads',d.counts.downloads],['Likes',d.counts.likes]];qs('#cards').innerHTML=cards.map(function(c){return '<div class="card"><span class="muted">'+c[0]+'</span><b>'+Number(c[1]||0)+'</b></div>';}).join('');qs('#dashPending').innerHTML=miniPresetList(d.newestPending||[]);qs('#dashTop').innerHTML=miniPresetList(d.topPresets||[]);}
function miniPresetList(rows){return rows.length?rows.map(function(r){return '<div class="row"><b>'+esc(r.title)+'</b> '+pill(r)+'<div class="muted">'+esc(r.id)+' by '+esc(r.author_name||'')+' '+client(r)+'</div><div class="muted">'+Number(r.downloads||0)+' downloads | '+Number(r.likes||0)+' likes</div></div>';}).join(''):'<div class="empty">Nothing here.</div>';}
async function loadReview(){const d=await api('/api/v1/admin/presets?status=pending&deleted=active&sort=created&limit=300');qs('#reviewTable').innerHTML=presetTable(d.presets||[],true);}
async function loadPresets(){const q=encodeURIComponent(qs('#presetSearch').value);const status=encodeURIComponent(qs('#presetStatus').value);const sort=encodeURIComponent(qs('#presetSort').value);const d=await api('/api/v1/admin/presets?q='+q+'&status='+status+'&sort='+sort+'&deleted=active&limit=300');qs('#presetTable').innerHTML=presetTable(d.presets||[],false);}
async function loadTrash(){const q=encodeURIComponent(qs('#trashSearch').value);const d=await api('/api/v1/admin/presets?q='+q+'&deleted=trash&sort=delete_after&limit=300');qs('#trashTable').innerHTML=presetTable(d.presets||[],true);}
function presetTable(rows,checks){return table([(checks?'<input type="checkbox" data-action="toggleAll">':''),'Preset','Client','Stats','Dates','Actions'],rows.map(function(r){const actions=[actionButton('view',r.id,'View'),r.status==='pending'&&!r.deleted_at?actionButton('approve',r.id,'Approve','good'):'',r.status==='pending'&&!r.deleted_at?actionButton('reject',r.id,'Reject'):'',!r.deleted_at?actionButton('whitelistPreset',r.id,'Whitelist'):'',!r.deleted_at?actionButton('delete',r.id,'Delete','danger'):'',r.deleted_at?actionButton('restore',r.id,'Restore','good'):'',r.deleted_at?actionButton('purge',r.id,'Purge','danger'):''].filter(Boolean).join(' ');return '<tr><td>'+(checks?selectCell(r):'')+'</td><td><b>'+esc(r.title)+'</b> '+pill(r)+(r.update_of?'<div class="muted">Update for '+esc(r.update_of)+'</div>':'')+'<div class="muted">'+esc(r.id)+' by '+esc(r.author_name||'')+'</div><div>'+esc(r.description||'')+'</div></td><td>'+client(r)+'</td><td class="nowrap">'+Number(r.downloads||0)+' downloads<br>'+Number(r.likes||0)+' likes</td><td class="muted">Created '+esc(r.created_at||'')+'<br>Updated '+esc(r.updated_at||'')+(r.deleted_at?'<br><span class="dangerText">Purge after '+esc(r.delete_after||'')+'</span>':'')+'</td><td class="nowrap">'+actions+'</td></tr>'; }),'No presets found.');}
async function loadClients(){const q=encodeURIComponent(qs('#clientSearch').value);const sort=encodeURIComponent(qs('#clientSort').value);const d=await api('/api/v1/admin/clients?q='+q+'&sort='+sort+'&limit=300');qs('#clientTable').innerHTML=table(['Client','Whitelist','Uploads','Stats','Actions'],(d.clients||[]).map(function(c){return '<tr><td><div class="clientHash">'+esc(c.client_fingerprint)+'</div><div class="muted">'+esc(c.submitter_hash)+'</div></td><td><b>'+esc(c.label||'(unlabeled)')+'</b><div class="muted">'+(Number(c.auto_approve||0)?'Auto approve':'Not trusted')+'</div><div>'+esc(c.note||'')+'</div></td><td>Approved '+Number(c.approved_count||0)+'<br>Pending '+Number(c.pending_count||0)+'<br>Rejected '+Number(c.rejected_count||0)+'<br>Deleted '+Number(c.deleted_count||0)+'</td><td>'+Number(c.total_downloads||0)+' downloads<br>'+Number(c.total_likes||0)+' likes<br><span class="muted">'+esc(c.last_upload||'')+'</span></td><td>'+actionButton('clientUploads',c.submitter_hash,'View Uploads')+' '+actionButton('removeWhitelist',c.submitter_hash,'Remove Trust','danger')+'</td></tr>'; }),'No clients found.');}
async function loadUpdate(){const d=await api('/api/v1/admin/update');const u=d.update||{};qs('#updateVersion').value=u.latestVersion||'';qs('#updateUrl').value=u.downloadPageUrl||'';qs('#updatePublished').value=u.publishedAt||'';qs('#updateCritical').checked=!!u.critical;qs('#updateChangelog').value=u.changelog||'';qs('#updateArtifactInfo').textContent=u.addonSha256?('Direct artifact: '+Number(u.addonSizeBytes||0)+' bytes | sha256 '+u.addonSha256+' | '+(u.addonR2Key||'')):'Direct artifact: not uploaded';qs('#updateSource').textContent='Source: '+(u.source||'default');}
async function saveUpdate(){const body={latestVersion:qs('#updateVersion').value,downloadPageUrl:qs('#updateUrl').value,publishedAt:qs('#updatePublished').value,critical:qs('#updateCritical').checked,changelog:qs('#updateChangelog').value};await api('/api/v1/admin/update',{method:'PUT',headers:{'content-type':'application/json'},body:JSON.stringify(body)});msg('Update notice saved');await loadUpdate();}
async function uploadUpdateArtifact(){const file=qs('#updateArtifact').files[0];if(!file){msg('Choose a .addon64 file first.');return;}const version=qs('#updateVersion').value;if(!version){msg('Set Latest Version before uploading.');return;}const r=await fetch('/api/v1/admin/update/artifact?version='+encodeURIComponent(version),{method:'PUT',headers:{'content-type':'application/octet-stream'},body:file});if(!r.ok)throw new Error(await r.text());const d=await r.json();msg('Uploaded .addon64 '+d.version+' sha256 '+d.addonSha256);qs('#updateArtifact').value='';await loadUpdate();}
async function loadAudit(){const d=await api('/api/v1/admin/audit?limit=200');qs('#auditTable').innerHTML=table(['Time','Action','Preset','Admin','Note'],(d.audit||[]).map(function(a){return '<tr><td>'+esc(a.created_at)+'</td><td>'+esc(a.action)+'</td><td>'+esc(a.preset_id||'')+'</td><td>'+esc(a.admin_email_or_token||'')+'</td><td>'+esc(a.note||'')+'</td></tr>'; }),'No audit entries.');}
async function viewPreset(id){const d=await api('/api/v1/admin/presets/'+encodeURIComponent(id));qs('#detailTitle').textContent=d.preset.title+' by '+d.preset.author_name;qs('#detailMeta').textContent=d.preset.id+' | '+d.preset.status+' | '+(d.preset.client_fingerprint||'no client')+' | '+d.preset.r2_key;qs('#detailJson').textContent=JSON.stringify(d.preset,null,2);qs('#detailIni').textContent=d.iniText||'(missing file)';qs('#detailScan').textContent=JSON.stringify(d.scan,null,2);qs('#detail').hidden=false;}
function closeDetail(){qs('#detail').hidden=true;}
async function approve(id){await api('/api/v1/admin/submissions/'+encodeURIComponent(id)+'/approve',{method:'POST'});msg('Approved '+id);await loadActive();}
async function rejectOne(id){const note=prompt('Reject note?')||'';await api('/api/v1/admin/submissions/'+encodeURIComponent(id)+'/reject',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({note})});msg('Rejected '+id);await loadActive();}
async function deleteOne(id){if(!confirm('Hide '+id+' now? It will move to Trash and auto-purge after 7 days.'))return;await api('/api/v1/admin/presets/'+encodeURIComponent(id),{method:'DELETE',headers:{'content-type':'application/json'},body:JSON.stringify({reason:'admin'})});msg('Moved to Trash '+id);closeDetail();await loadActive();}
async function restoreOne(id){await api('/api/v1/admin/presets/'+encodeURIComponent(id)+'/restore',{method:'POST'});msg('Restored '+id);await loadActive();}
async function purgeOne(id){if(!confirm('Permanently purge '+id+'? This deletes DB and R2 data now.'))return;await api('/api/v1/admin/presets/'+encodeURIComponent(id)+'/purge',{method:'POST'});msg('Purged '+id);closeDetail();await loadActive();}
async function whitelistPreset(id){const label=prompt('Trusted label?')||'';await api('/api/v1/admin/whitelist/from-preset/'+encodeURIComponent(id),{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({label})});msg('Submitter whitelisted');await loadActive();}
async function addWhitelist(){const value=qs('#wlClientId').value;const label=qs('#wlLabel').value;const note=qs('#wlNote').value;const body=/^[a-f0-9]{64}$/i.test(value.trim())?{submitterHash:value.trim(),label,note,autoApprove:true}:{clientId:value,label,note,autoApprove:true};await api('/api/v1/admin/whitelist',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)});qs('#wlClientId').value='';msg('Client whitelisted');await loadClients();}
async function removeWhitelist(hash){if(!confirm('Remove trusted client?'))return;await api('/api/v1/admin/whitelist/'+encodeURIComponent(hash),{method:'DELETE'});msg('Trusted client removed');await loadClients();}
async function bulk(action){const ids=selectedIds();if(!ids.length){msg('Select at least one row.');return;}if(action==='delete'&&!confirm('Move '+ids.length+' preset(s) to Trash for 7 days?'))return;if(action==='purge'&&!confirm('Permanently purge '+ids.length+' preset(s)?'))return;const body={};body[action]=ids;await api('/api/v1/admin/submissions/batch',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)});msg('Batch '+action+' complete');await loadActive();}
async function rebuild(){await api('/api/v1/admin/catalog/rebuild',{method:'POST'});msg('Catalog rebuilt');await loadDashboard();}
async function logout(){await api('/api/v1/admin/logout',{method:'POST'});location.reload();}
document.addEventListener('click',function(e){const el=e.target.closest('button,input[type=checkbox]');if(!el)return;const a=el.dataset.action;if(el.dataset.tab){setTab(el.dataset.tab).catch(function(err){msg(err.message);});return;}if(el.dataset.bulk){bulk(el.dataset.bulk).catch(function(err){msg(err.message);});return;}if(a==='toggleAll'){qsa('.rowcheck').forEach(function(c){c.checked=el.checked;});return;}const id=el.dataset.id;if(a==='refresh')loadActive().catch(function(err){msg(err.message);});else if(a==='loadPresets')loadPresets().catch(function(err){msg(err.message);});else if(a==='loadClients')loadClients().catch(function(err){msg(err.message);});else if(a==='loadUpdate')loadUpdate().catch(function(err){msg(err.message);});else if(a==='saveUpdate')saveUpdate().catch(function(err){msg(err.message);});else if(a==='uploadUpdateArtifact')uploadUpdateArtifact().catch(function(err){msg(err.message);});else if(a==='loadTrash')loadTrash().catch(function(err){msg(err.message);});else if(a==='loadAudit')loadAudit().catch(function(err){msg(err.message);});else if(a==='view')viewPreset(id).catch(function(err){msg(err.message);});else if(a==='approve')approve(id).catch(function(err){msg(err.message);});else if(a==='reject')rejectOne(id).catch(function(err){msg(err.message);});else if(a==='delete')deleteOne(id).catch(function(err){msg(err.message);});else if(a==='restore')restoreOne(id).catch(function(err){msg(err.message);});else if(a==='purge')purgeOne(id).catch(function(err){msg(err.message);});else if(a==='whitelistPreset')whitelistPreset(id).catch(function(err){msg(err.message);});else if(a==='clientUploads'){setTab('presets').then(function(){qs('#presetSearch').value=id.slice(0,12);qs('#presetStatus').value='';return loadPresets();}).catch(function(err){msg(err.message);});}else if(a==='removeWhitelist')removeWhitelist(id).catch(function(err){msg(err.message);});else if(a==='addWhitelist')addWhitelist().catch(function(err){msg(err.message);});else if(a==='rebuild')rebuild().catch(function(err){msg(err.message);});else if(a==='logout')logout().catch(function(err){msg(err.message);});else if(a==='closeDetail')closeDetail();});
setTab('dashboard').catch(function(err){msg(err.message);});
</script></body></html>`;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const method = request.method.toUpperCase();
    try {
      if (method === "GET" && url.pathname === "/api/v1/catalog") return await getCatalog(env);
      if (method === "GET" && url.pathname === "/api/v1/update") return await updateInfo(request, env);
      if (method === "GET" && url.pathname === "/api/v1/update/artifact") return await updateArtifact(request, env);
      if (method === "POST" && url.pathname === "/api/v1/presets") return await submitPreset(request, env);
      if (method === "GET" && url.pathname === "/api/v1/me/presets") return await listMyPresets(request, env);
      let match = url.pathname.match(/^\/api\/v1\/me\/presets\/([^/]+)\/update$/);
      if (method === "DELETE" && match) return await cancelMyPresetUpdate(request, env, match[1]);
      match = url.pathname.match(/^\/api\/v1\/me\/presets\/([^/]+)$/);
      if (method === "PUT" && match) return await updateMyPreset(request, env, match[1]);
      if (method === "DELETE" && match) return await deleteMyPreset(request, env, match[1]);
      match = url.pathname.match(/^\/api\/v1\/presets\/([^/]+)\/download$/);
      if (method === "GET" && match) return await downloadPreset(request, env, match[1]);
      match = url.pathname.match(/^\/api\/v1\/presets\/([^/]+)\/like$/);
      if (method === "POST" && match) return await toggleLike(request, env, match[1]);
      if (method === "POST" && url.pathname === "/api/v1/admin/login") return await loginAdmin(request, env);
      if (method === "POST" && url.pathname === "/api/v1/admin/logout") return logoutAdmin();
      if (url.pathname === "/admin" && method === "GET") {
        if (!(await isAdmin(request, env))) {
          if (!hasAdminLoginKey(request, env)) return text("Not found.", 404);
          return new Response(ADMIN_LOGIN_HTML, { headers: { "content-type": "text/html; charset=utf-8" } });
        }
        return new Response(ADMIN_HTML, { headers: { "content-type": "text/html; charset=utf-8" } });
      }
      if (url.pathname.startsWith("/api/v1/admin/")) {
        if (!(await isAdmin(request, env))) return bad("Unauthorized", 401);
        if (method === "GET" && url.pathname === "/api/v1/admin/overview") return await adminOverview(env);
        if (method === "GET" && url.pathname === "/api/v1/admin/update") return await adminGetUpdateSettings(env);
        if (method === "PUT" && url.pathname === "/api/v1/admin/update") return await adminSaveUpdateSettings(request, env);
        if (method === "PUT" && url.pathname === "/api/v1/admin/update/artifact") return await adminUploadUpdateArtifact(request, env);
        if (method === "GET" && url.pathname === "/api/v1/admin/presets") return await listAdminPresets(env, url.searchParams);
        if (method === "GET" && url.pathname === "/api/v1/admin/clients") return await listAdminClients(env, url.searchParams);
        if (method === "GET" && url.pathname === "/api/v1/admin/audit") return await listAdminAudit(env, url.searchParams);
        if (method === "GET" && url.pathname === "/api/v1/admin/submissions") return await listSubmissions(env, url.searchParams.get("status") || "pending");
        if (method === "GET" && url.pathname === "/api/v1/admin/submissions/export.zip") return await exportPending(env);
        if (method === "POST" && url.pathname === "/api/v1/admin/catalog/rebuild") return json({ ok: true, catalog: await rebuildCatalog(env) });
        if (method === "POST" && url.pathname === "/api/v1/admin/submissions/batch") return await batchSubmissions(request, env);
        if (method === "GET" && url.pathname === "/api/v1/admin/whitelist") return await listWhitelist(env);
        if (method === "POST" && url.pathname === "/api/v1/admin/whitelist") return await addWhitelist(request, env);
        match = url.pathname.match(/^\/api\/v1\/admin\/whitelist\/from-preset\/([^/]+)$/);
        if (method === "POST" && match) return await whitelistFromPreset(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/whitelist\/([a-fA-F0-9]{64})$/);
        if (method === "DELETE" && match) return await deleteWhitelist(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/presets\/([^/]+)\/restore$/);
        if (method === "POST" && match) return await restorePresetAdmin(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/presets\/([^/]+)\/purge$/);
        if (method === "POST" && match) return await purgePresetAdmin(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/presets\/([^/]+)$/);
        if (method === "GET" && match) return await adminPresetDetail(env, match[1]);
        if (method === "DELETE" && match) return await deletePresetAdmin(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/presets\/([^/]+)\/ini$/);
        if (method === "GET" && match) return await adminPresetIni(env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/submissions\/([^/]+)\/approve$/);
        if (method === "POST" && match) return await approveSubmission(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/submissions\/([^/]+)\/reject$/);
        if (method === "POST" && match) return await rejectSubmission(request, env, match[1]);
      }
      return bad("Not found.", 404);
    } catch (error) {
      return bad(error?.message || "Server error.", 500);
    }
  },
  async scheduled(_event, env, _ctx) {
    await purgeExpiredDeletedPresets(env);
  }
};
