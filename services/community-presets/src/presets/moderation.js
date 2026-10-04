import { bad, json } from "../http/responses.js";
import { addDaysIso, nowIso } from "../shared/time.js";
import { adminIdentity } from "../auth/admin-session.js";
import { rebuildCatalog } from "./catalog.js";
import { readJson } from "../http/request-validation.js";
import { sanitizeText } from "../shared/values.js";
import { newPresetObjectKey, releaseStagingIfReferencedStatement, stagePresetObject } from "../storage/preset-objects.js";
import { drainPresetObjectCleanup } from "../storage/preset-objects.js";
import { purgePresetRecord } from "../storage/presets-repository.js";

export async function approveSubmission(request, env, id) {
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
    if (target.status !== "approved") return bad("Target preset is no longer approved.", 409);
    const approvedKey = newPresetObjectKey("approved", target.id);
    const historyKey = newPresetObjectKey(`history/${target.id}`, id);
    const bytes = await object.arrayBuffer();
    await stagePresetObject(env, approvedKey, bytes);
    await stagePresetObject(env, historyKey, bytes);
    const approvedAt = target.approved_at || nowIso();
    const reviewedAt = nowIso();
    const retainUntil = addDaysIso(7);
    const results = await env.DB.batch([env.DB.prepare(
      `UPDATE presets SET title=?,author_name=?,description=?,tags_json=?,status='approved',r2_key=?,sha256=?,size_bytes=?,format_version=?,min_addon_version=?,safety_status=?,safety_summary=?,updated_at=?,approved_at=?,content_revision=content_revision+1
       WHERE id=? AND update_of='' AND status='approved' AND deleted_at IS NULL AND content_revision=? AND r2_key=?
         AND EXISTS (SELECT 1 FROM preset_object_staging WHERE r2_key=?)
         AND EXISTS (SELECT 1 FROM preset_object_staging WHERE r2_key=?)
         AND EXISTS (SELECT 1 FROM presets WHERE id=? AND status='pending' AND deleted_at IS NULL AND r2_key=? AND content_revision=?)
         AND NOT EXISTS (SELECT 1 FROM presets newer WHERE newer.update_of=? AND newer.status='pending' AND newer.deleted_at IS NULL
           AND (newer.created_at>? OR (newer.created_at=? AND newer.id>?)))`
    ).bind(
      row.title, row.author_name, row.description, row.tags_json, approvedKey, row.sha256, row.size_bytes,
      row.format_version, row.min_addon_version, row.safety_status, row.safety_summary, reviewedAt, approvedAt,
      target.id, target.content_revision, target.r2_key, approvedKey, historyKey, id, row.r2_key, row.content_revision,
      row.update_of, row.created_at || "", row.created_at || "", row.id
    ), env.DB.prepare(
      `UPDATE presets SET status='approved',r2_key=?,approved_at=?,updated_at=?,deleted_at=?,delete_after=?,deleted_by=?,delete_reason=?,content_revision=content_revision+1
       WHERE id=? AND status='pending' AND deleted_at IS NULL AND r2_key=? AND content_revision=?
         AND EXISTS (SELECT 1 FROM presets WHERE id=? AND r2_key=?)`
    ).bind(historyKey, reviewedAt, reviewedAt, reviewedAt, retainUntil, adminIdentity(request, env), `applied to ${target.id}`,
      id, row.r2_key, row.content_revision, target.id, approvedKey),
    env.DB.prepare(
      "INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) SELECT 'approve-update',?,?,?,? WHERE EXISTS (SELECT 1 FROM presets WHERE id=? AND r2_key=?)"
    ).bind(target.id, adminIdentity(request, env), id, reviewedAt, id, historyKey),
    env.DB.prepare(
      "INSERT INTO preset_object_cleanup (r2_key,created_at) SELECT ?,? WHERE EXISTS (SELECT 1 FROM presets WHERE id=? AND r2_key=?) ON CONFLICT(r2_key) DO NOTHING"
    ).bind(target.r2_key, reviewedAt, target.id, approvedKey),
    env.DB.prepare(
      "INSERT INTO preset_object_cleanup (r2_key,created_at) SELECT ?,? WHERE EXISTS (SELECT 1 FROM presets WHERE id=? AND r2_key=?) ON CONFLICT(r2_key) DO NOTHING"
    ).bind(row.r2_key, reviewedAt, id, historyKey),
    releaseStagingIfReferencedStatement(env, approvedKey, target.id),
    releaseStagingIfReferencedStatement(env, historyKey, id)]);
    if (!results[0].meta.changes) return bad("Submission changed while approval was being prepared. Retry.", 409);
    await rebuildCatalog(env);
    return json({ ok: true, id: target.id, updateId: id, status: "approved", retainedUntil: retainUntil });
  }
  const approvedKey = newPresetObjectKey("approved", id);
  await stagePresetObject(env, approvedKey, await object.arrayBuffer());
  const approvedAt = nowIso();
  const results = await env.DB.batch([env.DB.prepare(
    `UPDATE presets SET status='approved',r2_key=?,approved_at=?,updated_at=?,content_revision=content_revision+1
     WHERE id=? AND status='pending' AND deleted_at IS NULL AND r2_key=? AND content_revision=?
       AND EXISTS (SELECT 1 FROM preset_object_staging WHERE r2_key=?)`
  ).bind(approvedKey, approvedAt, approvedAt, id, row.r2_key, row.content_revision, approvedKey), env.DB.prepare(
    "INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) SELECT 'approve',?,?, '',? WHERE EXISTS (SELECT 1 FROM presets WHERE id=? AND r2_key=?)"
  ).bind(id, adminIdentity(request, env), approvedAt, id, approvedKey), env.DB.prepare(
    "INSERT INTO preset_object_cleanup (r2_key,created_at) SELECT ?,? WHERE EXISTS (SELECT 1 FROM presets WHERE id=? AND r2_key=?) ON CONFLICT(r2_key) DO NOTHING"
  ).bind(row.r2_key, approvedAt, id, approvedKey), releaseStagingIfReferencedStatement(env, approvedKey, id)]);
  if (!results[0].meta.changes) return bad("Submission changed while approval was being prepared. Retry.", 409);
  await rebuildCatalog(env);
  return json({ ok: true, id, status: "approved" });
}

export async function rejectSubmission(request, env, id) {
  const body = await readJson(request) || {};
  const row = await env.DB.prepare("SELECT id,r2_key,content_revision FROM presets WHERE id=? AND status='pending' AND deleted_at IS NULL").bind(id).first();
  if (!row) return bad("Pending submission not found.", 404);
  const reviewedAt = nowIso();
  const results = await env.DB.batch([env.DB.prepare(
    "UPDATE presets SET status='rejected',rejected_at=?,updated_at=?,content_revision=content_revision+1 WHERE id=? AND status='pending' AND deleted_at IS NULL AND r2_key=? AND content_revision=?"
  ).bind(reviewedAt, reviewedAt, id, row.r2_key, row.content_revision), env.DB.prepare(
    "INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) SELECT 'reject',?,?,?,? WHERE changes()>0"
  ).bind(id, adminIdentity(request, env), sanitizeText(body.note, 300), reviewedAt)]);
  if (!results[0].meta.changes) return bad("Pending submission not found.", 404);
  return json({ ok: true, id, status: "rejected" });
}

export async function softDeletePreset(request, env, id, reason = "") {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND deleted_at IS NULL").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const deletedAt = nowIso();
  const deleteAfter = addDaysIso(7);
  const note = sanitizeText(reason || row.status || "", 300);
  const results = await env.DB.batch([env.DB.prepare(
    "UPDATE presets SET deleted_at=?,delete_after=?,deleted_by=?,delete_reason=?,updated_at=?,content_revision=content_revision+1 WHERE id=? AND deleted_at IS NULL AND r2_key=? AND content_revision=?"
  ).bind(deletedAt, deleteAfter, adminIdentity(request, env), note, deletedAt, id, row.r2_key, row.content_revision), env.DB.prepare(
    "INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) SELECT 'soft-delete',?,?,?,? WHERE changes()>0"
  ).bind(id, adminIdentity(request, env), note, deletedAt)]);
  if (!results[0].meta.changes) return bad("Preset changed while deletion was being prepared. Retry.", 409);
  if (row.status === "approved" && !row.update_of) await rebuildCatalog(env);
  return json({ ok: true, id, deleted: true, deletedAt, deleteAfter });
}

export async function restorePresetAdmin(request, env, id) {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND deleted_at IS NOT NULL").bind(id).first();
  if (!row) return bad("Deleted preset not found.", 404);
  const restoredAt = nowIso();
  const results = await env.DB.batch([env.DB.prepare(
    "UPDATE presets SET deleted_at=NULL,delete_after=NULL,deleted_by='',delete_reason='',updated_at=?,content_revision=content_revision+1 WHERE id=? AND deleted_at IS NOT NULL AND r2_key=? AND content_revision=?"
  ).bind(restoredAt, id, row.r2_key, row.content_revision), env.DB.prepare(
    "INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) SELECT 'restore',?,?,?,? WHERE changes()>0"
  ).bind(id, adminIdentity(request, env), row.delete_reason || "", restoredAt)]);
  if (!results[0].meta.changes) return bad("Preset changed while restoration was being prepared. Retry.", 409);
  if (row.status === "approved" && !row.update_of) await rebuildCatalog(env);
  return json({ ok: true, id, restored: true });
}

export async function purgePresetAdmin(request, env, id, action = "purge") {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=?").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const purged = await purgePresetRecord(env, row, adminIdentity(request, env), action, row.status || "", nowIso());
  if (!purged) return bad("Preset changed while purge was being prepared. Retry.", 409);
  if (row.status === "approved" && !row.update_of) await rebuildCatalog(env);
  try {
    await drainPresetObjectCleanup(env);
  } catch (error) {
    console.error(JSON.stringify({ event: "preset_object_cleanup_deferred", errorType: error?.name || "Error" }));
  }
  return json({ ok: true, id, purged: true });
}

export async function deletePresetAdmin(request, env, id) {
  const body = await readJson(request) || {};
  return softDeletePreset(request, env, id, sanitizeText(body.reason || "admin", 300));
}
