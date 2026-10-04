import { nowIso } from "../shared/time.js";
import { rebuildCatalog } from "../presets/catalog.js";
import { purgePresetRecord } from "../storage/presets-repository.js";
import { drainAbandonedPresetStaging, drainPresetObjectCleanup } from "../storage/preset-objects.js";

export async function purgeExpiredDeletedPresets(env) {
  const now = nowIso();
  const { results } = await env.DB.prepare(
    "SELECT * FROM presets WHERE deleted_at IS NOT NULL AND delete_after IS NOT NULL AND delete_after<=? ORDER BY delete_after ASC LIMIT 100"
  ).bind(now).all();
  const rows = results || [];
  let purged = 0;
  let rebuilt = false;
  for (const row of rows) {
    const removed = await purgePresetRecord(env, row, "scheduled-worker", "auto-purge", row.status || "", nowIso(), now);
    if (removed) {
      if (row.status === "approved" && !row.update_of) rebuilt = true;
      purged += 1;
    }
  }
  if (rebuilt) await rebuildCatalog(env);
  let cleanupError;
  try {
    for (let batch = 0; batch < 10; batch += 1) {
      if (await drainPresetObjectCleanup(env) < 100) break;
    }
  } catch (error) {
    cleanupError = error;
  }
  try {
    const cutoff = new Date(Date.now() - 24 * 60 * 60 * 1000).toISOString();
    for (let batch = 0; batch < 10; batch += 1) {
      if (await drainAbandonedPresetStaging(env, cutoff) < 100) break;
    }
  } catch (error) {
    cleanupError ||= error;
  }
  if (cleanupError) throw cleanupError;
  return { ok: true, purged };
}
