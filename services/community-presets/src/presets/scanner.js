const PRESET_HEADER = "[CrimsonWeatherPreset]";
const CURRENT_FORMAT_VERSION = 6;
const ALLOWED_SECTIONS = new Set([
  "Meta", "Weather", "Time", "Cloud", "Experiment", "Celestial", "Atmosphere"
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
  "NativeFogEnabled", "NativeFog", "VolumeFogScatterColorEnabled",
  "VolumeFogScatterColorR", "VolumeFogScatterColorG", "VolumeFogScatterColorB",
  "VolumeFogScatterColorA", "MieScatterColorEnabled", "MieScatterColorR",
  "MieScatterColorG", "MieScatterColorB", "MieScatterColorA", "MieScaleHeightEnabled",
  "MieScaleHeight", "MieAerosolDensityEnabled", "MieAerosolDensity",
  "MieAerosolAbsorptionEnabled", "MieAerosolAbsorption", "HeightFogBaselineEnabled",
  "HeightFogBaseline", "HeightFogFalloffEnabled", "HeightFogFalloff", "NoFog", "Wind", "NoWind",
]);
const STRING_KEYS = new Set(["MoonTexture", "MilkywayTexture"]);

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
  const byteLength = new TextEncoder().encode(iniText).byteLength;
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
