// Public update routes used by the in-game updater.
import { bad, json } from "../shared/http.js";
import { compareVersions, normalizeVersion, sanitizeText } from "../shared/values.js";
import { UPDATE_CHANNEL, getUpdateSettings, hasArtifact } from "../shared/updates.js";

// GET /api/v1/update[?version=&channel=]: latest version, changelog and artifact info.
export async function updateInfo(request, env) {
  const url = new URL(request.url);
  const channel = sanitizeText(url.searchParams.get("channel") || request.headers.get("x-cw-channel") || UPDATE_CHANNEL, 20) || UPDATE_CHANNEL;
  const currentVersion = normalizeVersion(url.searchParams.get("version") || request.headers.get("x-cw-client-version") || "");
  const settings = await getUpdateSettings(env);
  const latest = settings.latestVersion;
  const artifact = hasArtifact(settings);
  return json({
    ok: true,
    channel,
    currentVersion,
    updateAvailable: currentVersion ? compareVersions(latest, currentVersion) > 0 : true,
    version: latest,
    title: `Crimson Weather ${latest}`,
    changelog: settings.changelog,
    downloadPageUrl: settings.downloadPageUrl,
    addonDownloadUrl: artifact ? `${url.origin}/api/v1/update/artifact?version=${encodeURIComponent(latest)}` : "",
    addonSha256: artifact ? settings.addonSha256 : "",
    addonSizeBytes: artifact ? settings.addonSizeBytes : 0,
    publishedAt: settings.publishedAt,
    critical: settings.critical,
  }, 200, { "cache-control": "public, max-age=300" });
}

// GET /api/v1/update/artifact[?version=]: the addon file for the latest version.
export async function updateArtifact(request, env) {
  const settings = await getUpdateSettings(env);
  const requested = normalizeVersion(new URL(request.url).searchParams.get("version") || "");
  if (requested && requested !== settings.latestVersion) return bad("Requested update artifact version is not available.", 404);
  if (!settings.addonR2Key || !/^[a-f0-9]{64}$/i.test(settings.addonSha256)) return bad("Update artifact is not configured.", 404);
  const file = await env.PRESETS.get(settings.addonR2Key);
  if (!file) return bad("Update artifact was not found.", 404);
  return new Response(file.body, {
    headers: {
      "content-type": "application/octet-stream",
      "content-disposition": `attachment; filename="CrimsonWeather-${settings.latestVersion}.addon64"`,
      "x-cw-addon-sha256": settings.addonSha256,
      "x-cw-addon-size-bytes": String(settings.addonSizeBytes || ""),
      "cache-control": "no-store",
    },
  });
}
