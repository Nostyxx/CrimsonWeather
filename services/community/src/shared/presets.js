// Preset storage and lifecycle.
//
// A preset is a row in `presets` plus its INI file in R2 at `r2_key`:
//   pending/<id>/<rev>.ini                submitted, awaiting review
//   approved/<id>/<rev>.ini               live
//   history/<target>/<update>/preset.ini  an approved owner update, kept 7 days
//
// Every new content gets a new key (<rev> is random), so a file is never
// overwritten in place. A change writes the new file first, then switches the
// row to it with a conditional UPDATE that checks the row still has the key
// (and hash) the change started from. If something else changed the row in
// between, the UPDATE matches nothing, the new file is deleted and the caller
// gets a conflict. Old files are deleted only after the row stops using them.
//
// The original Worker always reads files through `r2_key`, so it can still
// serve everything written here (rollback stays possible).
//
// Owner updates to an approved preset are separate rows with update_of=<target>;
// approving one copies its content onto the target. Deletes are soft for
// RETENTION_DAYS, then the scheduled job purges rows and files.
import { rebuildCatalog } from "./catalog.js";
import { audit } from "./audit.js";
import { clientFingerprint, sha256Hex } from "./identity.js";
import { addDaysIso, nowIso, sanitizeText } from "./values.js";

export const RETENTION_DAYS = 7;

const revision = () => crypto.randomUUID().replace(/-/g, "").slice(0, 12);
export const newPendingKey = (id) => `pending/${id}/${revision()}.ini`;
export const newApprovedKey = (id) => `approved/${id}/${revision()}.ini`;
export const historyKey = (targetId, updateId) => `history/${targetId}/${updateId}/preset.ini`;

// Whether a change to this row can change the public catalog.
export const isRootPreset = (row) => !row.update_of;

export function putPresetFile(env, key, bytes) {
  return env.PRESETS.put(key, bytes, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
}

// Deletes files the database no longer references. Best effort: a leftover
// file is only wasted space, never served.
export async function deleteFiles(env, ...keys) {
  const list = keys.filter(Boolean);
  if (!list.length) return;
  try {
    await env.PRESETS.delete(list);
  } catch (error) {
    console.error(JSON.stringify({ event: "file_cleanup_failed", keys: list, error: String(error) }));
  }
}

// Runs the database change that makes newly written files live. If it throws,
// the new files are removed again (nothing references them).
export async function commitOrRemoveFiles(env, newKeys, commit) {
  try {
    return await commit();
  } catch (error) {
    await deleteFiles(env, ...newKeys);
    throw error;
  }
}

export async function getPreset(env, id) {
  return env.DB.prepare("SELECT * FROM presets WHERE id=?").bind(id).first();
}

// Deletes a preset's row, but only if it is still exactly the row the caller
// read (same file, plus any extra condition). Likes and download records go
// with it (foreign keys cascade). Returns whether this call removed it.
async function deleteRowIfUnchanged(env, row, extraWhere = "", extraBinds = []) {
  const removed = await env.DB.prepare(`DELETE FROM presets WHERE id=? AND r2_key=? AND sha256=? ${extraWhere}`)
    .bind(row.id, row.r2_key, row.sha256, ...extraBinds).run();
  return removed.meta.changes > 0;
}

// Removes every file of a purged preset: all its revisions, the legacy fixed
// keys and whatever its row pointed at.
async function deletePresetFiles(env, row) {
  const keys = new Set([row.r2_key, `pending/${row.id}/preset.ini`, `approved/${row.id}/preset.ini`]);
  for (const prefix of [`pending/${row.id}/`, `approved/${row.id}/`]) {
    try {
      const listed = await env.PRESETS.list({ prefix });
      for (const object of listed.objects) keys.add(object.key);
    } catch (error) {
      console.error(JSON.stringify({ event: "file_list_failed", prefix, error: String(error) }));
    }
  }
  await deleteFiles(env, ...keys);
}

// Scheduled: purge soft-deleted presets whose retention has expired.
export async function purgeExpiredPresets(env) {
  const now = nowIso();
  const { results } = await env.DB.prepare(
    `SELECT * FROM presets
      WHERE deleted_at IS NOT NULL AND delete_after IS NOT NULL AND delete_after<=?
      ORDER BY delete_after ASC LIMIT 100`,
  ).bind(now).all();
  let purged = 0;
  let republish = false;
  for (const row of results || []) {
    // Re-checked in the DELETE itself: a restore since the SELECT wins.
    if (!(await deleteRowIfUnchanged(env, row, "AND deleted_at IS NOT NULL AND delete_after IS NOT NULL AND delete_after<=?", [now]))) continue;
    purged++;
    await deletePresetFiles(env, row);
    await audit(env, "scheduled-worker", "auto-purge", row.id, row.status || "");
    republish ||= isRootPreset(row);
  }
  if (republish) await rebuildCatalog(env);
  return { purged };
}

// ---------------------------------------------------------------------------
// Moderation (admin actions). `actor` identifies who acted, e.g. "admin:me@x".
// Each returns { ok: true, ... } or { ok: false, status, error }.
// ---------------------------------------------------------------------------
const fail = (status, error) => ({ ok: false, status, error });
const CHANGED = "This preset changed while you were working on it. Reload and check it again.";

// Approves a pending submission or owner update. `expectedSha256` (optional)
// is the content the moderator reviewed; anything else is not approved.
export async function approve(env, actor, id, expectedSha256 = "") {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND status='pending' AND deleted_at IS NULL").bind(id).first();
  if (!row) return fail(404, "Pending submission not found.");
  if (expectedSha256 && expectedSha256 !== row.sha256) return fail(409, CHANGED);
  const file = await env.PRESETS.get(row.r2_key);
  if (!file) return fail(404, "Pending preset file missing.");
  const bytes = await file.arrayBuffer();
  if ((await sha256Hex(new Uint8Array(bytes))) !== row.sha256) return fail(409, CHANGED);
  return row.update_of ? approveUpdate(env, actor, row, bytes) : approveNew(env, actor, row, bytes);
}

async function approveNew(env, actor, row, bytes) {
  const key = newApprovedKey(row.id);
  const now = nowIso();
  await putPresetFile(env, key, bytes);
  const updated = await commitOrRemoveFiles(env, [key], () => env.DB.prepare(
    `UPDATE presets SET status='approved', r2_key=?, approved_at=?, updated_at=?
      WHERE id=? AND status='pending' AND deleted_at IS NULL AND r2_key=? AND sha256=?`,
  ).bind(key, now, now, row.id, row.r2_key, row.sha256).run());
  if (!updated.meta.changes) {
    await deleteFiles(env, key);
    return fail(409, CHANGED);
  }
  await audit(env, actor, "approve", row.id);
  await deleteFiles(env, row.r2_key);
  await rebuildCatalog(env);
  return { ok: true, id: row.id, status: "approved" };
}

// Applies an owner's update to the live preset it targets.
async function approveUpdate(env, actor, row, bytes) {
  const newerPending = `SELECT id FROM presets
      WHERE update_of=? AND status='pending' AND deleted_at IS NULL
        AND (created_at > ? OR (created_at = ? AND id > ?))`;
  const newerBinds = [row.update_of, row.created_at || "", row.created_at || "", row.id];
  const newer = await env.DB.prepare(`${newerPending} ORDER BY created_at DESC, id DESC LIMIT 1`).bind(...newerBinds).first();
  if (newer) return fail(409, `A newer pending update exists for this preset: ${newer.id}`);
  const target = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND update_of='' AND deleted_at IS NULL").bind(row.update_of).first();
  if (!target) return fail(404, "Target preset for update was not found.");

  const liveKey = newApprovedKey(target.id);
  const keptKey = historyKey(target.id, row.id);
  await putPresetFile(env, liveKey, bytes);
  await putPresetFile(env, keptKey, bytes);
  const now = nowIso();
  const retainUntil = addDaysIso(RETENTION_DAYS);
  // One transaction. The target changes only if it, and the update, are still
  // what was read above and no newer update arrived; the update row is marked
  // applied only if the target now points at this approval's new file.
  const [applied] = await commitOrRemoveFiles(env, [liveKey, keptKey], () => env.DB.batch([
    env.DB.prepare(
      `UPDATE presets SET title=?,author_name=?,description=?,tags_json=?,status='approved',r2_key=?,sha256=?,size_bytes=?,
              format_version=?,min_addon_version=?,safety_status=?,safety_summary=?,updated_at=?,approved_at=?
        WHERE id=? AND update_of='' AND deleted_at IS NULL AND r2_key=? AND sha256=?
          AND EXISTS (SELECT 1 FROM presets u WHERE u.id=? AND u.status='pending' AND u.deleted_at IS NULL AND u.r2_key=? AND u.sha256=?)
          AND NOT EXISTS (${newerPending})`,
    ).bind(row.title, row.author_name, row.description, row.tags_json, liveKey, row.sha256, row.size_bytes,
      row.format_version, row.min_addon_version, row.safety_status, row.safety_summary, now, target.approved_at || now,
      target.id, target.r2_key, target.sha256, row.id, row.r2_key, row.sha256, ...newerBinds),
    // The update row is kept (soft-deleted) as a record of what was applied.
    env.DB.prepare(
      `UPDATE presets SET status='approved',r2_key=?,approved_at=?,updated_at=?,deleted_at=?,delete_after=?,deleted_by=?,delete_reason=?
        WHERE id=? AND status='pending' AND deleted_at IS NULL AND EXISTS (SELECT 1 FROM presets t WHERE t.id=? AND t.r2_key=?)`,
    ).bind(keptKey, now, now, now, retainUntil, actor, `applied to ${target.id}`, row.id, target.id, liveKey),
  ]));
  if (!applied.meta.changes) {
    await deleteFiles(env, liveKey, keptKey);
    return fail(409, CHANGED);
  }
  await audit(env, actor, "approve-update", target.id, row.id);
  await deleteFiles(env, row.r2_key, target.r2_key);
  await rebuildCatalog(env);
  return { ok: true, id: target.id, updateId: row.id, status: "approved", retainedUntil: retainUntil };
}

export async function reject(env, actor, id, note = "") {
  const now = nowIso();
  const rejected = await env.DB.prepare(
    "UPDATE presets SET status='rejected', rejected_at=?, updated_at=? WHERE id=? AND status='pending' AND deleted_at IS NULL",
  ).bind(now, now, id).run();
  if (!rejected.meta.changes) return fail(404, "Pending submission not found.");
  await audit(env, actor, "reject", id, sanitizeText(note, 300));
  return { ok: true, id, status: "rejected" };
}

// Hides a preset now; it is purged after RETENTION_DAYS unless restored.
export async function softDelete(env, actor, id, reason = "") {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND deleted_at IS NULL").bind(id).first();
  if (!row) return fail(404, "Preset not found.");
  const deletedAt = nowIso();
  const deleteAfter = addDaysIso(RETENTION_DAYS);
  const note = sanitizeText(reason || row.status || "", 300);
  const deleted = await env.DB.prepare(
    "UPDATE presets SET deleted_at=?, delete_after=?, deleted_by=?, delete_reason=?, updated_at=? WHERE id=? AND deleted_at IS NULL",
  ).bind(deletedAt, deleteAfter, actor, note, deletedAt, id).run();
  if (!deleted.meta.changes) return fail(404, "Preset not found.");
  await audit(env, actor, "soft-delete", id, note);
  // Rebuilt even if it was not live when read: it may have been approved since.
  if (isRootPreset(row)) await rebuildCatalog(env);
  return { ok: true, id, deleted: true, deletedAt, deleteAfter };
}

export async function restore(env, actor, id) {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND deleted_at IS NOT NULL").bind(id).first();
  if (!row) return fail(404, "Deleted preset not found.");
  // Conditional: if the purge removed the row meanwhile, nothing is restored.
  const restored = await env.DB.prepare(
    "UPDATE presets SET deleted_at=NULL, delete_after=NULL, deleted_by='', delete_reason='', updated_at=? WHERE id=? AND deleted_at IS NOT NULL",
  ).bind(nowIso(), id).run();
  if (!restored.meta.changes) return fail(404, "Deleted preset not found.");
  await audit(env, actor, "restore", id, row.delete_reason || "");
  if (isRootPreset(row)) await rebuildCatalog(env);
  return { ok: true, id, restored: true };
}

// Permanently removes a preset now (rows and files).
export async function purge(env, actor, id) {
  const row = await getPreset(env, id);
  if (!row) return fail(404, "Preset not found.");
  if (!(await deleteRowIfUnchanged(env, row))) return fail(409, CHANGED);
  await deletePresetFiles(env, row);
  await audit(env, actor, "purge", id, row.status || "");
  if (isRootPreset(row)) await rebuildCatalog(env);
  return { ok: true, id, purged: true };
}

// Trusts the uploader of a preset: their future uploads are approved automatically.
export async function trustSubmitterOf(env, actor, id, label = "", note = "") {
  const row = await env.DB.prepare("SELECT id,title,author_name,submitter_hash FROM presets WHERE id=?").bind(id).first();
  if (!row || !row.submitter_hash) return fail(404, "Preset submitter was not found.");
  const now = nowIso();
  const finalLabel = sanitizeText(label, 80) || row.author_name || row.title || id;
  await env.DB.prepare(
    `INSERT INTO client_whitelist (submitter_hash,label,auto_approve,note,created_at,updated_at) VALUES (?,?,1,?,?,?)
     ON CONFLICT(submitter_hash) DO UPDATE SET label=excluded.label,auto_approve=1,note=excluded.note,updated_at=excluded.updated_at`,
  ).bind(row.submitter_hash, finalLabel, sanitizeText(note, 300), now, now).run();
  await audit(env, actor, "whitelist-from-preset", id, clientFingerprint(row.submitter_hash));
  return { ok: true, submitterHash: row.submitter_hash, fingerprint: clientFingerprint(row.submitter_hash), label: finalLabel };
}
