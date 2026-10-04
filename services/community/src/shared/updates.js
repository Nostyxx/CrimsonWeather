// Addon update notice and artifact (what the in-game updater checks).
//
// Stored as `update.*` rows in app_settings; the keys are shared with the
// original Worker so either can read what the other wrote.
import { sanitizeText, normalizeVersion, nowIso } from "./values.js";
import {
  DEFAULT_UPDATE_CHANNEL,
  DEFAULT_LATEST_VERSION,
  DEFAULT_DOWNLOAD_PAGE_URL,
  DEFAULT_CHANGELOG,
} from "./update-defaults.js";

export const UPDATE_CHANNEL = DEFAULT_UPDATE_CHANNEL;
// The addon is under 1 MB. The upload is held in memory to hash it, and a
// Worker has 128 MB for everything, so keep this well below that.
export const ARTIFACT_MAX_BYTES = 32 * 1024 * 1024;

const KEYS = {
  latestVersion: "update.latestVersion",
  downloadPageUrl: "update.downloadPageUrl",
  changelog: "update.changelog",
  publishedAt: "update.publishedAt",
  critical: "update.critical",
  addonR2Key: "update.addonR2Key",
  addonSha256: "update.addonSha256",
  addonSizeBytes: "update.addonSizeBytes",
};

// Environment variables may override the built-in defaults (not the stored settings).
function defaults(env) {
  return {
    channel: UPDATE_CHANNEL,
    latestVersion: normalizeVersion(env.UPDATE_LATEST_VERSION || DEFAULT_LATEST_VERSION),
    downloadPageUrl: sanitizeText(env.UPDATE_DOWNLOAD_PAGE_URL || DEFAULT_DOWNLOAD_PAGE_URL, 300),
    addonR2Key: sanitizeText(env.UPDATE_ADDON_R2_KEY || "", 300),
    addonSha256: sanitizeText(env.UPDATE_ADDON_SHA256 || "", 80).toLowerCase(),
    addonSizeBytes: Number(env.UPDATE_ADDON_SIZE_BYTES || 0) || 0,
    changelog: String(env.UPDATE_CHANGELOG || DEFAULT_CHANGELOG),
    publishedAt: String(env.UPDATE_PUBLISHED_AT || ""),
    critical: String(env.UPDATE_CRITICAL || "0") === "1",
    source: "default",
  };
}

export async function getUpdateSettings(env) {
  const settings = defaults(env);
  const { results } = await env.DB.prepare("SELECT key,value FROM app_settings WHERE key LIKE 'update.%'").all();
  const stored = Object.fromEntries((results || []).map((r) => [r.key, String(r.value ?? "")]));
  const get = (name) => stored[KEYS[name]] || "";

  if (Object.values(KEYS).some((key) => stored[key])) settings.source = "database";
  if (get("latestVersion")) settings.latestVersion = normalizeVersion(get("latestVersion"));
  if (get("downloadPageUrl")) settings.downloadPageUrl = sanitizeText(get("downloadPageUrl"), 300);
  if (get("changelog")) settings.changelog = get("changelog");
  if (get("publishedAt")) settings.publishedAt = get("publishedAt").slice(0, 80);
  if (get("critical")) settings.critical = get("critical") === "1" || get("critical") === "true";
  if (get("addonR2Key")) settings.addonR2Key = sanitizeText(get("addonR2Key"), 300);
  if (get("addonSha256")) settings.addonSha256 = sanitizeText(get("addonSha256"), 80).toLowerCase();
  if (get("addonSizeBytes")) settings.addonSizeBytes = Number(get("addonSizeBytes")) || 0;
  return settings;
}

export const hasArtifact = (s) => !!(s.addonR2Key && /^[a-f0-9]{64}$/i.test(s.addonSha256) && s.addonSizeBytes > 0);

// Writes several update.* settings in one batch (all or nothing).
export async function saveUpdateSettings(env, values, actor) {
  const now = nowIso();
  const stmt = env.DB.prepare(
    "INSERT INTO app_settings (key,value,updated_at,updated_by) VALUES (?,?,?,?) " +
      "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at, updated_by=excluded.updated_by",
  );
  const statements = Object.entries(values).map(([name, value]) => {
    if (!KEYS[name]) throw new Error(`unknown update setting ${name}`);
    return stmt.bind(KEYS[name], String(value ?? ""), now, actor);
  });
  if (statements.length) await env.DB.batch(statements);
}
