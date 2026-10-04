// Preset validation. Uploaded presets are INI text that other players load
// into the addon, so only known sections/keys and plain values are accepted.
//
// The allow-lists are the production Worker's lists, verbatim: presets saved by
// any shipped addon version (including RenoDX and legacy Fog keys) must stay valid.
// Extend them when a new addon version adds preset keys.

export const PRESET_HEADER = "[CrimsonWeatherPreset]";
export const CURRENT_FORMAT_VERSION = 6;

const ALLOWED_SECTIONS = new Set([
  "Meta", "Weather", "Time", "Cloud",
  "Experiment", "Celestial", "Atmosphere", "RenoDX",
]);

const ALLOWED_KEYS = new Set([
  "FormatVersion", "Enabled", "ForceClearSky", "NoRain",
  "Rain", "Thunder", "NoDust", "Dust",
  "NoSnow", "Snow", "SnowAccumBoundaryAEnabled", "SnowAccumBoundaryA",
  "SnowAccumBoundaryBEnabled", "SnowAccumBoundaryB", "SnowCoverageThresholdEnabled", "SnowCoverageThreshold",
  "VisualTimeOverride", "ProgressVisualTime", "ProgressVisualTimeMatchGameTime", "ProgressVisualTimeIntervalMs",
  "TimeHour", "CloudAmountEnabled", "CloudAmount", "CloudHeightEnabled",
  "CloudHeight", "CloudDensityEnabled", "CloudDensity", "MidCloudsEnabled",
  "MidClouds", "HighCloudLayerEnabled", "HighCloudLayer", "CloudAlphaEnabled",
  "CloudAlpha", "CloudFadeRangeEnabled", "CloudFadeRange", "CloudDetailRatioEnabled",
  "CloudDetailRatio", "CloudPhaseFrontEnabled", "CloudPhaseFront", "CloudScatteringCoefficientEnabled",
  "CloudScatteringCoefficient", "CloudFlowEnabled", "CloudFlow", "CloudVisibleRangeEnabled",
  "CloudVisibleRange", "RayleighHeightEnabled", "RayleighHeight", "OzoneRatioEnabled",
  "OzoneRatio", "RayleighScatteringColorEnabled", "RayleighScatteringColorR", "RayleighScatteringColorG",
  "RayleighScatteringColorB", "2CEnabled", "2C", "2DEnabled",
  "2D", "CloudVariationEnabled", "CloudVariation", "PuddleScaleEnabled",
  "PuddleScale", "NightSkyTiltEnabled", "NightSkyTilt", "NightSkyPhaseEnabled",
  "NightSkyPhase", "SunSizeEnabled", "SunSize", "SunLightIntensityEnabled",
  "SunLightIntensity", "SunYawEnabled", "SunYaw", "SunPitchEnabled",
  "SunPitch", "MoonSizeEnabled", "MoonSize", "MoonLightIntensityEnabled",
  "MoonLightIntensity", "MoonYawEnabled", "MoonYaw", "MoonPitchEnabled",
  "MoonPitch", "MoonRollEnabled", "MoonRoll", "MoonTextureEnabled",
  "MoonTexture", "MilkywayTextureEnabled", "MilkywayTexture", "FogEnabled",
  "Fog", "NativeFogEnabled", "NativeFog", "VolumeFogScatterColorEnabled",
  "VolumeFogScatterColorR", "VolumeFogScatterColorG", "VolumeFogScatterColorB", "VolumeFogScatterColorA",
  "MieScatterColorEnabled", "MieScatterColorR", "MieScatterColorG", "MieScatterColorB",
  "MieScatterColorA", "MieScaleHeightEnabled", "MieScaleHeight", "MieAerosolDensityEnabled",
  "MieAerosolDensity", "MieAerosolAbsorptionEnabled", "MieAerosolAbsorption", "HeightFogBaselineEnabled",
  "HeightFogBaseline", "HeightFogFalloffEnabled", "HeightFogFalloff", "NoFog",
  "Wind", "NoWind", "AuroraEnabled", "AuroraGateEnabled",
  "RenoDxAuroraEnabled", "AuroraRegionMask", "RenoDxAuroraRegionMask", "RenoDXAuroraRegionMask",
]);

// Keys whose values are file names rather than numbers/booleans.
const STRING_KEYS = new Set(["MoonTexture", "MilkywayTexture"]);

const BOOLEAN_WORDS = new Set(["0", "1", "true", "false", "yes", "no", "on", "off"]);

const CONTROL_CHARS = /[\u0000-\u0008\u000b\u000c\u000e-\u001f]/;

function formatVersionOf(lines) {
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

// Values that could point outside the preset: URLs, parent paths, absolute/UNC paths.
function isUnsafeValue(value) {
  return /https?:\/\//i.test(value) ||
    /(^|[\\/])\.\.([\\/]|$)/.test(value) ||
    /^[a-z]:[\\/]/i.test(value) ||
    /^\\\\/.test(value);
}

// Returns { ok, errors[], warnings[], formatVersion }; errors are shown to the uploader.
export function scanPresetIni(iniText, maxBytes = 65536) {
  const result = { ok: true, errors: [], warnings: [], formatVersion: 0 };
  if (typeof iniText !== "string" || !iniText.trim()) {
    result.ok = false;
    result.errors.push("Preset text is empty.");
    return result;
  }
  if (new TextEncoder().encode(iniText).byteLength > maxBytes) result.errors.push(`Preset exceeds ${maxBytes} bytes.`);
  if (CONTROL_CHARS.test(iniText)) result.errors.push("Preset contains control characters.");

  const lines = iniText.replace(/^\uFEFF/, "").split(/\r?\n/).map((line) => line.trim());
  if (lines[0] !== PRESET_HEADER) result.errors.push("Missing [CrimsonWeatherPreset] header.");
  result.formatVersion = formatVersionOf(lines);
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
      // Region overrides are "[Region.<name>]"; "[Region.<x>.<Section>]" is also accepted.
      const base = section.startsWith("Region.") ? section.split(".").slice(-1)[0] : section;
      if (!ALLOWED_SECTIONS.has(base) && !/^Region\.[A-Za-z0-9_ -]+$/.test(section)) {
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
    if (isUnsafeValue(value)) result.errors.push(`Unsafe value for ${key}.`);
    if (!STRING_KEYS.has(key) && key !== "Enabled" && value !== "" && !BOOLEAN_WORDS.has(value.toLowerCase())) {
      if (!Number.isFinite(Number(value))) result.errors.push(`Non-finite numeric value for ${key}.`);
    }
  }
  result.ok = result.errors.length === 0;
  return result;
}
