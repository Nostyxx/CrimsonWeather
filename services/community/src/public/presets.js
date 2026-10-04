// Public preset routes used by the addon: upload, "My Uploads", download, like.
import { bad, json, readJson } from "../shared/http.js";
import { clientFingerprint, clientIdFrom, hashClientId, isValidClientId, sha256Hex } from "../shared/identity.js";
import { scanPresetIni } from "../shared/preset-scan.js";
import { rebuildCatalog } from "../shared/catalog.js";
import { audit } from "../shared/audit.js";
import { RETENTION_DAYS, commitOrRemoveFiles, deleteFiles, isRootPreset, newApprovedKey, newPendingKey, putPresetFile } from "../shared/presets.js";
import { addDaysIso, isVersion, normalizeVersion, nowIso, sanitizeText, slugify } from "../shared/values.js";

const missingClient = () => bad("Missing anonymous client id.", 400);

// The submitter hash of the calling client, or "" if it sent no usable id.
async function submitterOf(request, env) {
  const clientId = clientIdFrom(request);
  return isValidClientId(clientId) ? hashClientId(env, clientId) : "";
}

async function isTrusted(env, submitterHash) {
  return !!(await env.DB.prepare("SELECT 1 FROM client_whitelist WHERE submitter_hash=? AND auto_approve=1").bind(submitterHash).first());
}

// Validated fields of an upload/edit body, or an error response.
function parseUpload(body, env) {
  const title = sanitizeText(body.title, 80);
  if (!title) return { error: bad("Title is required.") };
  const iniText = String(body.iniText || "");
  const scan = scanPresetIni(iniText, Number(env.MAX_PRESET_BYTES || 65536));
  if (!scan.ok) return { error: bad("Preset validation failed.", 422, scan) };
  const clientVersion = normalizeVersion(body.clientVersion);
  return {
    title,
    author: sanitizeText(body.authorName, 40) || "Anonymous",
    description: sanitizeText(body.description, 500),
    tagsJson: "[]",
    bytes: new TextEncoder().encode(iniText),
    scan,
    // Recorded as the minimum addon version able to load the preset.
    minAddonVersion: isVersion(clientVersion) ? clientVersion : env.MIN_ADDON_VERSION || "0.6.3",
  };
}

const newId = (title, infix = "") => `${slugify(title)}-${infix}${crypto.randomUUID().slice(0, 8)}`;
const autoApprovedNote = (hash) => `Auto-approved whitelist ${clientFingerprint(hash)}`;

// POST /api/v1/presets
export async function submitPreset(request, env) {
  const body = await readJson(request);
  if (!body) return bad("Invalid JSON.");
  const clientId = clientIdFrom(request);
  if (!isValidClientId(clientId)) return missingClient();
  const upload = parseUpload(body, env);
  if (upload.error) return upload.error;

  const id = newId(upload.title);
  const submitterHash = await hashClientId(env, clientId);
  const trusted = await isTrusted(env, submitterHash);
  const status = trusted ? "approved" : "pending";
  const key = trusted ? newApprovedKey(id) : newPendingKey(id);
  const created = nowIso();
  const hash = await sha256Hex(upload.bytes);
  await putPresetFile(env, key, upload.bytes);
  await commitOrRemoveFiles(env, [key], () => env.DB.prepare(
    `INSERT INTO presets (id,title,author_name,description,tags_json,status,r2_key,sha256,size_bytes,format_version,
       min_addon_version,submitter_hash,safety_status,safety_summary,created_at,updated_at,approved_at)
     VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`,
  ).bind(id, upload.title, upload.author, upload.description, upload.tagsJson, status, key, hash,
    upload.bytes.byteLength, upload.scan.formatVersion || 0, upload.minAddonVersion, submitterHash, "passed",
    trusted ? autoApprovedNote(submitterHash) : upload.scan.warnings.join("; "), created, created, trusted ? created : null).run());
  if (trusted) await rebuildCatalog(env);
  return json({ ok: true, id, status, autoApproved: trusted, scan: upload.scan });
}

// GET /api/v1/presets/:id/download
export async function downloadPreset(request, env, id) {
  const row = await env.DB.prepare("SELECT id,r2_key FROM presets WHERE id=? AND status='approved' AND deleted_at IS NULL").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const file = await env.PRESETS.get(row.r2_key);
  if (!file) return bad("Preset file missing.", 404);

  // Count at most one download per device per day.
  const clientId = clientIdFrom(request);
  if (clientId) {
    const deviceHash = await hashClientId(env, clientId);
    const day = nowIso().slice(0, 10);
    // The record and the counter change in one transaction: the counter moves
    // only if this request's record was new (changes() of the INSERT).
    const now = nowIso();
    const [inserted] = await env.DB.batch([
      env.DB.prepare("INSERT OR IGNORE INTO preset_downloads_daily (preset_id,device_hash,day,created_at) VALUES (?,?,?,?)")
        .bind(id, deviceHash, day, now),
      env.DB.prepare("UPDATE presets SET downloads=downloads+1, updated_at=? WHERE id=? AND changes()=1").bind(now, id),
    ]);
    if (inserted.meta.changes > 0) await rebuildCatalog(env);
  }
  return new Response(file.body, {
    headers: { "content-type": "text/plain; charset=utf-8", "content-disposition": `attachment; filename="${id}.ini"` },
  });
}

// POST /api/v1/presets/:id/like (toggles the caller's like)
export async function toggleLike(request, env, id) {
  const row = await env.DB.prepare("SELECT id FROM presets WHERE id=? AND status='approved' AND deleted_at IS NULL").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const clientId = clientIdFrom(request);
  if (!clientId) return missingClient();
  const deviceHash = await hashClientId(env, clientId);
  const now = nowIso();

  // Unlike if liked, otherwise like. Each step changes the like and its
  // counter in one transaction, and the counter moves only if the like row
  // actually changed (changes() of the previous statement).
  const [removed] = await env.DB.batch([
    env.DB.prepare("DELETE FROM preset_likes WHERE preset_id=? AND device_hash=?").bind(id, deviceHash),
    env.DB.prepare("UPDATE presets SET likes=MAX(0, likes-1), updated_at=? WHERE id=? AND changes()=1").bind(now, id),
  ]);
  const liked = removed.meta.changes === 0;
  if (liked) {
    await env.DB.batch([
      env.DB.prepare("INSERT OR IGNORE INTO preset_likes (preset_id,device_hash,created_at) VALUES (?,?,?)").bind(id, deviceHash, now),
      env.DB.prepare("UPDATE presets SET likes=likes+1, updated_at=? WHERE id=? AND changes()=1").bind(now, id),
    ]);
  }
  await rebuildCatalog(env);
  const updated = await env.DB.prepare("SELECT likes FROM presets WHERE id=?").bind(id).first();
  return json({ ok: true, liked, likes: updated?.likes || 0 });
}

// GET /api/v1/me/presets: the caller's live presets, each with its newest pending update.
export async function listMyPresets(request, env) {
  const submitterHash = await submitterOf(request, env);
  if (!submitterHash) return missingClient();
  const { results } = await env.DB.prepare(
    `SELECT
       p.id,p.title,p.author_name,p.description,p.tags_json,p.status,p.update_of,p.downloads,p.likes,
       p.created_at,p.updated_at,p.approved_at,p.rejected_at,
       u.id AS pending_update_id,
       u.title AS pending_update_title,
       u.updated_at AS pending_update_at
     FROM presets p
     LEFT JOIN presets u ON u.update_of=p.id AND u.status='pending' AND u.deleted_at IS NULL
       AND NOT EXISTS (
         SELECT 1 FROM presets newer
          WHERE newer.update_of=p.id AND newer.status='pending' AND newer.deleted_at IS NULL
            AND (newer.created_at > u.created_at OR (newer.created_at = u.created_at AND newer.id > u.id))
       )
     WHERE p.submitter_hash=? AND p.update_of='' AND p.deleted_at IS NULL
     ORDER BY p.updated_at DESC LIMIT 100`,
  ).bind(submitterHash).all();
  return json({ ok: true, clientFingerprint: clientFingerprint(submitterHash), presets: results || [] });
}

// DELETE /api/v1/me/presets/:id
export async function deleteMyPreset(request, env, id) {
  const submitterHash = await submitterOf(request, env);
  if (!submitterHash) return missingClient();
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND submitter_hash=? AND deleted_at IS NULL").bind(id, submitterHash).first();
  if (!row) return bad("Preset not found for this client.", 404);
  const now = nowIso();
  const deleted = await env.DB.prepare(
    "UPDATE presets SET deleted_at=?, delete_after=?, deleted_by=?, delete_reason=?, updated_at=? WHERE id=? AND deleted_at IS NULL",
  ).bind(now, addDaysIso(RETENTION_DAYS), `client:${clientFingerprint(submitterHash)}`, "client delete", now, id).run();
  if (!deleted.meta.changes) return bad("Preset not found for this client.", 404);
  // Rebuilt even if it was not live when read: it may have been approved since.
  if (isRootPreset(row)) await rebuildCatalog(env);
  return json({ ok: true, id, deleted: true });
}

// DELETE /api/v1/me/presets/:id/update: withdraws the newest pending update.
export async function cancelMyPresetUpdate(request, env, id) {
  const submitterHash = await submitterOf(request, env);
  if (!submitterHash) return missingClient();
  const target = await env.DB.prepare("SELECT id FROM presets WHERE id=? AND submitter_hash=? AND update_of='' AND deleted_at IS NULL")
    .bind(id, submitterHash).first();
  if (!target) return bad("Preset not found for this client.", 404);
  const row = await env.DB.prepare(
    `SELECT id FROM presets WHERE update_of=? AND submitter_hash=? AND status='pending' AND deleted_at IS NULL
      ORDER BY created_at DESC, id DESC LIMIT 1`,
  ).bind(id, submitterHash).first();
  if (!row) return bad("Pending update not found for this preset.", 404);
  const now = nowIso();
  const retainUntil = addDaysIso(RETENTION_DAYS);
  const cancelled = await env.DB.prepare(
    `UPDATE presets SET status='rejected',rejected_at=?,deleted_at=?,delete_after=?,deleted_by=?,delete_reason=?,updated_at=?
      WHERE id=? AND status='pending' AND deleted_at IS NULL`,
  ).bind(now, now, retainUntil, `client:${clientFingerprint(submitterHash)}`, "client cancelled update", now, row.id).run();
  if (!cancelled.meta.changes) return bad("Pending update not found for this preset.", 404);
  await audit(env, "system", "client-cancel-update", row.id, `target=${id}`);
  return json({ ok: true, id, updateId: row.id, cancelled: true, retainedUntil: retainUntil });
}

// PUT /api/v1/me/presets/:id: edits a preset. Unapproved presets (and trusted
// uploaders) are edited in place; changes to a live preset become a pending
// update that replaces any earlier pending update.
export async function updateMyPreset(request, env, id) {
  const submitterHash = await submitterOf(request, env);
  if (!submitterHash) return missingClient();
  const target = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND submitter_hash=? AND update_of='' AND deleted_at IS NULL")
    .bind(id, submitterHash).first();
  if (!target) return bad("Preset not found for this client.", 404);
  const body = await readJson(request);
  if (!body) return bad("Invalid JSON.");
  const upload = parseUpload(body, env);
  if (upload.error) return upload.error;

  const hash = await sha256Hex(upload.bytes);
  const trusted = await isTrusted(env, submitterHash);
  const now = nowIso();

  if (target.status !== "approved" || trusted) {
    const key = target.status === "approved" ? newApprovedKey(id) : newPendingKey(id);
    await putPresetFile(env, key, upload.bytes);
    // Only if the preset is still in the state read above: a pending preset
    // approved meanwhile must not take unreviewed content.
    const edited = await commitOrRemoveFiles(env, [key], () => env.DB.prepare(
      `UPDATE presets SET title=?,author_name=?,description=?,tags_json=?,r2_key=?,sha256=?,size_bytes=?,format_version=?,
              min_addon_version=?,safety_status=?,safety_summary=?,updated_at=?,approved_at=COALESCE(approved_at, ?)
        WHERE id=? AND submitter_hash=? AND update_of='' AND deleted_at IS NULL AND status=? AND r2_key=? AND sha256=?`,
    ).bind(upload.title, upload.author, upload.description, upload.tagsJson, key, hash, upload.bytes.byteLength,
      upload.scan.formatVersion || 0, upload.minAddonVersion, "passed",
      trusted ? autoApprovedNote(submitterHash) : upload.scan.warnings.join("; "), now,
      target.status === "approved" ? now : null, id, submitterHash, target.status, target.r2_key, target.sha256).run());
    if (!edited.meta.changes) {
      await deleteFiles(env, key);
      return bad("This preset changed while saving (it may have just been reviewed). Please try again.", 409);
    }
    await deleteFiles(env, target.r2_key);
    if (target.status === "approved") await rebuildCatalog(env);
    return json({ ok: true, id, status: target.status, autoApproved: trusted, scan: upload.scan });
  }

  const updateId = newId(upload.title, "update-");
  const key = newPendingKey(updateId);
  await putPresetFile(env, key, upload.bytes);
  // One transaction: add this update, supersede the older pending ones, and
  // supersede this one instead if a newer one was added concurrently. Exactly
  // one pending update (the newest) remains.
  const retainUntil = addDaysIso(RETENTION_DAYS);
  const pendingOf = "update_of=? AND status='pending' AND deleted_at IS NULL";
  const supersede = "status='rejected',rejected_at=?,deleted_at=?,delete_after=?,deleted_by='system',updated_at=?";
  const newer = `SELECT n.id FROM presets n WHERE n.update_of=? AND n.status='pending' AND n.deleted_at IS NULL
      AND (n.created_at > ? OR (n.created_at = ? AND n.id > ?)) ORDER BY n.created_at DESC, n.id DESC LIMIT 1`;
  await commitOrRemoveFiles(env, [key], () => env.DB.batch([
    env.DB.prepare(
      `INSERT INTO presets (id,title,author_name,description,tags_json,status,r2_key,sha256,size_bytes,format_version,
         min_addon_version,submitter_hash,safety_status,safety_summary,created_at,updated_at,update_of)
       VALUES (?,?,?,?,?,'pending',?,?,?,?,?,?,?,?,?,?,?)`,
    ).bind(updateId, upload.title, upload.author, upload.description, upload.tagsJson, key, hash, upload.bytes.byteLength,
      upload.scan.formatVersion || 0, upload.minAddonVersion, submitterHash, "passed", upload.scan.warnings.join("; "),
      now, now, id),
    env.DB.prepare(
      `UPDATE presets SET ${supersede},delete_reason=?
        WHERE ${pendingOf} AND (created_at < ? OR (created_at = ? AND id < ?))`,
    ).bind(now, now, retainUntil, now, `superseded by ${updateId}`, id, now, now, updateId),
    env.DB.prepare(
      `UPDATE presets SET ${supersede},delete_reason='superseded by ' || (${newer})
        WHERE id=? AND status='pending' AND EXISTS (${newer})`,
    ).bind(now, now, retainUntil, now, id, now, now, updateId, updateId, id, now, now, updateId),
  ]));

  const { results: superseded } = await env.DB.prepare("SELECT id,delete_reason FROM presets WHERE update_of=? AND delete_reason=?")
    .bind(id, `superseded by ${updateId}`).all();
  for (const old of superseded || []) await audit(env, "system", "supersede-update", old.id, `replaced by ${updateId}`);
  const mine = await env.DB.prepare("SELECT status,delete_reason FROM presets WHERE id=?").bind(updateId).first();
  if (mine?.status !== "pending") {
    await audit(env, "system", "supersede-update", updateId, String(mine?.delete_reason || "").replace(/^superseded/, "replaced"));
    return bad("A newer update for this preset was sent at the same time. Check My Uploads.", 409);
  }
  return json({ ok: true, id, updateId, status: "pending", autoApproved: false, scan: upload.scan });
}
