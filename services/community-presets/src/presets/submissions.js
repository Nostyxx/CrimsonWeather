import { readJson } from "../http/request-validation.js";
import { bad, json } from "../http/responses.js";
import { bytesOf, sanitizeText, slugify } from "../shared/values.js";
import { scanPresetIni } from "./scanner.js";
import { sha256Hex } from "../shared/crypto.js";
import { addDaysIso, nowIso } from "../shared/time.js";
import { clientFingerprint, hashClientId, submitterHashFromRequest } from "../auth/client-identity.js";
import { whitelistRow } from "../storage/clients-repository.js";
import { rebuildCatalog } from "./catalog.js";
import { newPresetObjectKey, releaseStagingIfReferencedStatement, stagePresetObject } from "../storage/preset-objects.js";

export async function submitPreset(request, env) {
  const body = await readJson(request);
  if (!body) return bad("Invalid JSON.");
  const clientId = request.headers.get("x-cw-client-id") || "";
  if (!clientId || clientId.length > 128) return bad("Missing anonymous client id.", 400);
  const title = sanitizeText(body.title, 80);
  const author = sanitizeText(body.authorName, 40) || "Anonymous";
  const description = sanitizeText(body.description, 500);
  const tags = [];
  const iniText = String(body.iniText || "");
  const maxBytes = Number(env.MAX_PRESET_BYTES || 65536);
  if (!title) return bad("Title is required.");
  const scan = scanPresetIni(iniText, maxBytes);
  if (!scan.ok) return bad("Preset validation failed.", 422, scan);
  const bytes = bytesOf(iniText);
  const hash = await sha256Hex(bytes);
  const id = `${slugify(title)}-${crypto.randomUUID().slice(0, 8)}`;
  const created = nowIso();
  const submitterHash = await hashClientId(env, clientId);
  const trusted = await whitelistRow(env, submitterHash);
  const status = trusted ? "approved" : "pending";
  const r2Key = newPresetObjectKey(status, id);
  await stagePresetObject(env, r2Key, bytes);
  const trustedGuard = trusted ? " AND EXISTS (SELECT 1 FROM client_whitelist WHERE submitter_hash=? AND auto_approve=1)" : "";
  const statements = [env.DB.prepare(
    `INSERT INTO presets (id,title,author_name,description,tags_json,status,r2_key,sha256,size_bytes,format_version,min_addon_version,submitter_hash,safety_status,safety_summary,created_at,updated_at,approved_at) SELECT ?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,? WHERE EXISTS (SELECT 1 FROM preset_object_staging WHERE r2_key=?)${trustedGuard}`
  ).bind(
    id, title, author, description, JSON.stringify(tags), status, r2Key, hash, bytes.byteLength,
    scan.formatVersion || 0, body.clientVersion || env.MIN_ADDON_VERSION || "0.6.3",
    submitterHash, "passed", trusted ? `Auto-approved whitelist ${clientFingerprint(submitterHash)}` : scan.warnings.join("; "), created, created,
    trusted ? created : null, r2Key, ...(trusted ? [submitterHash] : [])
  ), releaseStagingIfReferencedStatement(env, r2Key, id)];
  const results = await env.DB.batch(statements);
  if (!results[0].meta.changes) return bad("Submission changed while it was being prepared. Retry.", 409);
  if (trusted) await rebuildCatalog(env);
  return json({ ok: true, id, status, autoApproved: Boolean(trusted), scan });
}

export async function listMyPresets(request, env) {
  const submitterHash = await submitterHashFromRequest(request, env);
  if (!submitterHash) return bad("Missing anonymous client id.", 400);
  const { results } = await env.DB.prepare(
    `SELECT
       p.id,p.title,p.author_name,p.description,p.tags_json,p.status,p.update_of,p.downloads,p.likes,p.created_at,p.updated_at,p.approved_at,p.rejected_at,
       u.id AS pending_update_id,
       u.title AS pending_update_title,
       u.updated_at AS pending_update_at
     FROM presets p
     LEFT JOIN presets u ON u.update_of=p.id AND u.status='pending' AND u.deleted_at IS NULL
       AND NOT EXISTS (
         SELECT 1 FROM presets newer
         WHERE newer.update_of=p.id
           AND newer.status='pending'
           AND newer.deleted_at IS NULL
           AND (
             newer.created_at > u.created_at
             OR (newer.created_at = u.created_at AND newer.id > u.id)
           )
       )
     WHERE p.submitter_hash=? AND p.update_of='' AND p.deleted_at IS NULL
     ORDER BY p.updated_at DESC LIMIT 100`
  ).bind(submitterHash).all();
  return json({ ok: true, clientFingerprint: clientFingerprint(submitterHash), presets: results || [] });
}

export async function deleteMyPreset(request, env, id) {
  const submitterHash = await submitterHashFromRequest(request, env);
  if (!submitterHash) return bad("Missing anonymous client id.", 400);
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND submitter_hash=? AND deleted_at IS NULL").bind(id, submitterHash).first();
  if (!row) return bad("Preset not found for this client.", 404);
  const deletedAt = nowIso();
  const result = await env.DB.prepare(
    "UPDATE presets SET deleted_at=?,delete_after=?,deleted_by=?,delete_reason=?,updated_at=?,content_revision=content_revision+1 WHERE id=? AND submitter_hash=? AND deleted_at IS NULL AND content_revision=? AND r2_key=?"
  ).bind(deletedAt, addDaysIso(7), `client:${clientFingerprint(submitterHash)}`, "client delete", deletedAt,
    id, submitterHash, row.content_revision, row.r2_key).run();
  if (!result.meta.changes) return bad("Preset changed while deletion was being prepared. Retry.", 409);
  if (row.status === "approved" && !row.update_of) await rebuildCatalog(env);
  return json({ ok: true, id, deleted: true });
}

export async function cancelMyPresetUpdate(request, env, id) {
  const submitterHash = await submitterHashFromRequest(request, env);
  if (!submitterHash) return bad("Missing anonymous client id.", 400);
  const target = await env.DB.prepare("SELECT id FROM presets WHERE id=? AND submitter_hash=? AND update_of='' AND deleted_at IS NULL").bind(id, submitterHash).first();
  if (!target) return bad("Preset not found for this client.", 404);
  const row = await env.DB.prepare(
    `SELECT * FROM presets
     WHERE update_of=? AND submitter_hash=? AND status='pending' AND deleted_at IS NULL
     ORDER BY created_at DESC, id DESC LIMIT 1`
  ).bind(id, submitterHash).first();
  if (!row) return bad("Pending update not found for this preset.", 404);
  const cancelledAt = nowIso();
  const retainUntil = addDaysIso(7);
  const results = await env.DB.batch([env.DB.prepare(
    `UPDATE presets SET status='rejected',rejected_at=?,deleted_at=?,delete_after=?,deleted_by=?,delete_reason=?,updated_at=?,content_revision=content_revision+1
     WHERE id=? AND update_of=? AND submitter_hash=? AND status='pending' AND deleted_at IS NULL AND content_revision=? AND r2_key=?
       AND EXISTS (SELECT 1 FROM presets WHERE id=? AND submitter_hash=? AND update_of='' AND deleted_at IS NULL)`
  ).bind(cancelledAt, cancelledAt, retainUntil, `client:${clientFingerprint(submitterHash)}`, "client cancelled update", cancelledAt,
    row.id, id, submitterHash, row.content_revision, row.r2_key, id, submitterHash), env.DB.prepare(
    "INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) SELECT 'client-cancel-update',?,'system',?,? WHERE changes()>0"
  ).bind(row.id, `target=${id}`, cancelledAt)]);
  if (!results[0].meta.changes) return bad("Pending update not found for this preset.", 404);
  return json({ ok: true, id, updateId: row.id, cancelled: true, retainedUntil: retainUntil });
}

export async function updateMyPreset(request, env, id) {
  const submitterHash = await submitterHashFromRequest(request, env);
  if (!submitterHash) return bad("Missing anonymous client id.", 400);
  const target = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND submitter_hash=? AND update_of='' AND deleted_at IS NULL").bind(id, submitterHash).first();
  if (!target) return bad("Preset not found for this client.", 404);
  const body = await readJson(request);
  if (!body) return bad("Invalid JSON.");
  const title = sanitizeText(body.title, 80);
  const author = sanitizeText(body.authorName, 40) || "Anonymous";
  const description = sanitizeText(body.description, 500);
  const tags = [];
  const iniText = String(body.iniText || "");
  const maxBytes = Number(env.MAX_PRESET_BYTES || 65536);
  if (!title) return bad("Title is required.");
  const scan = scanPresetIni(iniText, maxBytes);
  if (!scan.ok) return bad("Preset validation failed.", 422, scan);
  const bytes = bytesOf(iniText);
  const hash = await sha256Hex(bytes);
  const trusted = await whitelistRow(env, submitterHash);
  const updated = nowIso();

  if (target.status !== "approved" || trusted) {
    const r2Key = newPresetObjectKey(target.status, id);
    await stagePresetObject(env, r2Key, bytes);
    const trustedGuard = trusted ? " AND EXISTS (SELECT 1 FROM client_whitelist WHERE submitter_hash=? AND auto_approve=1)" : "";
    const results = await env.DB.batch([env.DB.prepare(
      `UPDATE presets SET title=?,author_name=?,description=?,tags_json=?,r2_key=?,sha256=?,size_bytes=?,format_version=?,min_addon_version=?,safety_status=?,safety_summary=?,updated_at=?,approved_at=COALESCE(approved_at, ?),content_revision=content_revision+1 WHERE id=? AND submitter_hash=? AND status=? AND deleted_at IS NULL AND content_revision=? AND r2_key=? AND EXISTS (SELECT 1 FROM preset_object_staging WHERE r2_key=?)${trustedGuard}`
    ).bind(
      title, author, description, JSON.stringify(tags), r2Key, hash, bytes.byteLength,
      scan.formatVersion || 0, body.clientVersion || env.MIN_ADDON_VERSION || "0.6.3",
      "passed", trusted ? `Auto-approved whitelist ${clientFingerprint(submitterHash)}` : scan.warnings.join("; "),
      updated, target.status === "approved" ? updated : null, id, submitterHash, target.status, target.content_revision, target.r2_key, r2Key,
      ...(trusted ? [submitterHash] : [])
    ), env.DB.prepare(
      "INSERT INTO preset_object_cleanup (r2_key,created_at) SELECT ?,? WHERE changes()>0 ON CONFLICT(r2_key) DO NOTHING"
    ).bind(target.r2_key, updated), releaseStagingIfReferencedStatement(env, r2Key, id)]);
    if (!results[0].meta.changes) return bad("Preset changed while this update was being prepared. Retry.", 409);
    if (target.status === "approved") await rebuildCatalog(env);
    return json({ ok: true, id, status: target.status, autoApproved: Boolean(trusted), scan });
  }

  const updateId = `${slugify(title)}-update-${crypto.randomUUID().slice(0, 8)}`;
  const r2Key = newPresetObjectKey("pending", updateId);
  await stagePresetObject(env, r2Key, bytes);
  const retainUntil = addDaysIso(7);
  const results = await env.DB.batch([env.DB.prepare(
    `INSERT INTO presets (id,title,author_name,description,tags_json,status,r2_key,sha256,size_bytes,format_version,min_addon_version,submitter_hash,safety_status,safety_summary,created_at,updated_at,update_of)
     SELECT ?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,? WHERE EXISTS (SELECT 1 FROM preset_object_staging WHERE r2_key=?)
       AND EXISTS (SELECT 1 FROM presets WHERE id=? AND submitter_hash=? AND update_of='' AND deleted_at IS NULL AND status=? AND content_revision=? AND r2_key=?)`
  ).bind(
    updateId, title, author, description, JSON.stringify(tags), "pending", r2Key, hash, bytes.byteLength,
    scan.formatVersion || 0, body.clientVersion || env.MIN_ADDON_VERSION || "0.6.3",
    submitterHash, "passed", scan.warnings.join("; "), updated, updated, id,
    r2Key, id, submitterHash, target.status, target.content_revision, target.r2_key
  ), env.DB.prepare(
    `INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at)
     SELECT 'supersede-update',id,'system',?,? FROM presets
     WHERE update_of=? AND status='pending' AND deleted_at IS NULL AND id<>?
       AND EXISTS (SELECT 1 FROM presets WHERE id=? AND r2_key=?)`
  ).bind(`replaced by ${updateId}`, updated, id, updateId, updateId, r2Key), env.DB.prepare(
    `UPDATE presets SET status='rejected',rejected_at=?,deleted_at=?,delete_after=?,deleted_by='system',delete_reason=?,updated_at=?
     WHERE update_of=? AND status='pending' AND deleted_at IS NULL AND id<>?
       AND EXISTS (SELECT 1 FROM presets WHERE id=? AND r2_key=?)`
  ).bind(updated, updated, retainUntil, `superseded by ${updateId}`, updated, id, updateId, updateId, r2Key),
  releaseStagingIfReferencedStatement(env, r2Key, updateId)]);
  if (!results[0].meta.changes) return bad("Preset changed while this update was being prepared. Retry.", 409);
  return json({ ok: true, id, updateId, status: "pending", autoApproved: false, scan });
}
