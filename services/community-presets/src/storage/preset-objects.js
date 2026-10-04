import { nowIso } from "../shared/time.js";

export function newPresetObjectKey(status, id) {
  return `${status}/${id}/revisions/${crypto.randomUUID()}/preset.ini`;
}

// The staging row exists before the R2 write. A failed or interrupted D1
// publication therefore leaves a durable, discoverable cleanup candidate.
export async function stagePresetObject(env, key, bytes) {
  await env.DB.prepare("INSERT INTO preset_object_staging (r2_key,created_at) VALUES (?,?)")
    .bind(key, nowIso()).run();
  await env.PRESETS.put(key, bytes, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
}

export function releaseStagingIfReferencedStatement(env, key, id) {
  return env.DB.prepare(
    "DELETE FROM preset_object_staging WHERE r2_key=? AND EXISTS (SELECT 1 FROM presets WHERE id=? AND r2_key=?)"
  ).bind(key, id, key);
}

// Keep the object if any current preset row still names it. All new writes use
// unique keys, so a queued old key cannot become a new active revision.
export async function drainPresetObjectCleanup(env, limit = 100) {
  const { results } = await env.DB.prepare(
    "SELECT q.r2_key FROM preset_object_cleanup q WHERE NOT EXISTS (SELECT 1 FROM presets p WHERE p.r2_key=q.r2_key) ORDER BY q.created_at,q.r2_key LIMIT ?"
  )
    .bind(limit).all();
  let deleted = 0;
  let firstError;
  for (const { r2_key: key } of results || []) {
    const active = await env.DB.prepare("SELECT 1 FROM presets WHERE r2_key=? LIMIT 1").bind(key).first();
    if (active) continue;
    try {
      await env.PRESETS.delete(key);
      await env.DB.prepare("DELETE FROM preset_object_cleanup WHERE r2_key=?").bind(key).run();
      deleted += 1;
    } catch (error) {
      firstError ||= error;
    }
  }
  if (firstError) throw firstError;
  return deleted;
}

// Only a staging row older than a day can be reclaimed. Removing that row
// first prevents a delayed request from publishing it after the R2 deletion.
export async function drainAbandonedPresetStaging(env, cutoffIso, limit = 100) {
  const { results } = await env.DB.prepare("SELECT r2_key FROM preset_object_staging WHERE created_at<=? ORDER BY created_at,r2_key LIMIT ?")
    .bind(cutoffIso, limit).all();
  let deleted = 0;
  let firstError;
  for (const { r2_key: key } of results || []) {
    const moved = await env.DB.batch([env.DB.prepare(
      `INSERT INTO preset_object_cleanup (r2_key,created_at)
       SELECT r2_key,? FROM preset_object_staging WHERE r2_key=? AND created_at<=?
         AND NOT EXISTS (SELECT 1 FROM presets WHERE r2_key=?)
       ON CONFLICT(r2_key) DO NOTHING`
    ).bind(nowIso(), key, cutoffIso, key), env.DB.prepare(
      `DELETE FROM preset_object_staging WHERE r2_key=? AND created_at<=?
         AND NOT EXISTS (SELECT 1 FROM presets WHERE r2_key=?)
         AND EXISTS (SELECT 1 FROM preset_object_cleanup WHERE r2_key=?)`
    ).bind(key, cutoffIso, key, key)]);
    if (!moved[1].meta.changes) continue;
    try {
      await env.PRESETS.delete(key);
      await env.DB.prepare("DELETE FROM preset_object_cleanup WHERE r2_key=?").bind(key).run();
      deleted += 1;
    } catch (error) {
      firstError ||= error;
    }
  }
  if (firstError) throw firstError;
  return deleted;
}
