import { UPDATE_CHANGELOG, UPDATE_CHANNEL, UPDATE_DOWNLOAD_PAGE_URL, UPDATE_LATEST_VERSION } from "./defaults.js";
import { compareVersions, normalizeVersion } from "../shared/version.js";
import { sanitizeText } from "../shared/values.js";
import { getUpdateSettingValues, publishUpdateSettings } from "../storage/settings-repository.js";
import { bad, json } from "../http/responses.js";
import { readJson } from "../http/request-validation.js";

export function defaultUpdateSettings(env) {
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

export async function getUpdateSettings(env) {
  const settings = defaultUpdateSettings(env);
  const values = await getUpdateSettingValues(env);
  const rows = ["latestVersion", "downloadPageUrl", "changelog", "publishedAt", "critical", "addonR2Key", "addonSha256", "addonSizeBytes"]
    .map((key) => values.get(`update.${key}`) ?? "");
  if (rows.some((value) => value !== "")) settings.source = "database";
  if (rows[0]) settings.latestVersion = normalizeVersion(rows[0]);
  if (rows[1]) settings.downloadPageUrl = sanitizeText(rows[1], 300);
  if (rows[2]) settings.changelog = String(rows[2]);
  if (rows[3]) settings.publishedAt = String(rows[3]).slice(0, 80);
  if (rows[4]) settings.critical = rows[4] === "1" || rows[4] === "true";
  // Explicitly cleared DB values must not revive environment fallback artifacts.
  if (values.has("update.addonR2Key")) settings.addonR2Key = sanitizeText(rows[5], 300);
  if (values.has("update.addonSha256")) settings.addonSha256 = sanitizeText(rows[6], 80).toLowerCase();
  if (values.has("update.addonSizeBytes")) settings.addonSizeBytes = Number(rows[7]) || 0;
  return settings;
}

export async function updateInfo(request, env) {
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

export async function adminGetUpdateSettings(env) {
  return json({ ok: true, update: await getUpdateSettings(env) }, 200, { "cache-control": "no-store" });
}

export async function adminSaveUpdateSettings(request, env) {
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
  await publishUpdateSettings(env, request, {
    "update.latestVersion": latestVersion,
    "update.downloadPageUrl": downloadPageUrl,
    "update.changelog": changelog,
    "update.publishedAt": publishedAt,
    "update.critical": critical
  }, "update-settings", `latest=${latestVersion}`, {
    version: latestVersion, defaultVersion: defaultUpdateSettings(env).latestVersion
  });
  return adminGetUpdateSettings(env);
}
