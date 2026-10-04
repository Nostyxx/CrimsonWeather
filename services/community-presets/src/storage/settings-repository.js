import { nowIso } from "../shared/time.js";
import { adminIdentity } from "../auth/admin-session.js";

export async function getUpdateSettingValues(env) {
  try {
    // One statement reads a coherent publication rather than eight independent reads.
    const { results } = await env.DB.prepare("SELECT key,value FROM app_settings WHERE key LIKE 'update.%'").all();
    return new Map((results || []).map((row) => [row.key, String(row.value ?? "")]));
  } catch {
    return new Map();
  }
}

function settingStatement(env, key, value, at, identity) {
  return env.DB.prepare(
    "INSERT INTO app_settings (key,value,updated_at,updated_by) VALUES (?,?,?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at, updated_by=excluded.updated_by"
  ).bind(key, String(value ?? ""), at, identity);
}

export async function publishUpdateSettings(env, request, values, action, note, resetArtifact = undefined) {
  const at = nowIso();
  const identity = adminIdentity(request, env);
  const statements = [];
  if (resetArtifact) {
    // Evaluate the version inside the same transaction, before writing the new one.
    // This avoids clearing an artifact from a concurrent same-version upload.
    for (const [key, value] of [["update.addonR2Key", ""], ["update.addonSha256", ""], ["update.addonSizeBytes", "0"]]) {
      statements.push(env.DB.prepare(
        `INSERT INTO app_settings (key,value,updated_at,updated_by)
         SELECT ?,?,?,? WHERE COALESCE((SELECT NULLIF(value,'') FROM app_settings WHERE key='update.latestVersion'),?) <> ?
         ON CONFLICT(key) DO UPDATE SET value=excluded.value,updated_at=excluded.updated_at,updated_by=excluded.updated_by`
      ).bind(key, value, at, identity, resetArtifact.defaultVersion, resetArtifact.version));
    }
  }
  for (const [key, value] of Object.entries(values)) statements.push(settingStatement(env, key, value, at, identity));
  statements.push(env.DB.prepare("INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) VALUES (?,'',?,?,?)")
    .bind(action, identity, note, at));
  await env.DB.batch(statements);
}
