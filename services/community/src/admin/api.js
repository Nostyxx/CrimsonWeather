// Admin API handlers. Every function receives the authenticated `actor`.
import { bad, json, readBody, readJson } from "../shared/http.js";
import { clientFingerprint, hashClientId, sha256Hex } from "../shared/identity.js";
import { scanPresetIni } from "../shared/preset-scan.js";
import { rebuildCatalog } from "../shared/catalog.js";
import { audit, auditStatement } from "../shared/audit.js";
import * as lifecycle from "../shared/presets.js";
import { ARTIFACT_MAX_BYTES, getUpdateSettings, saveUpdateSettings } from "../shared/updates.js";
import { isVersion, normalizeVersion, nowIso, sanitizeText } from "../shared/values.js";

const result = (outcome) => (outcome.ok ? json(outcome) : bad(outcome.error, outcome.status));
const limitOf = (value, fallback = 100, max = 300) => {
  const n = Number.parseInt(value || "", 10);
  return Number.isFinite(n) && n > 0 ? Math.min(n, max) : fallback;
};
const withFingerprint = (row) => ({ ...row, client_fingerprint: clientFingerprint(row.submitter_hash || "") });

// ---------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------
export async function overview(env) {
  const count = (sql) => env.DB.prepare(`SELECT COUNT(*) AS n FROM presets WHERE ${sql}`).first("n");
  const [pending, approved, rejected, trash, totals, clients, trusted] = await Promise.all([
    count("status='pending' AND deleted_at IS NULL"),
    count("status='approved' AND update_of='' AND deleted_at IS NULL"),
    count("status='rejected' AND deleted_at IS NULL"),
    count("deleted_at IS NOT NULL AND update_of=''"),
    env.DB.prepare("SELECT COALESCE(SUM(downloads),0) AS downloads, COALESCE(SUM(likes),0) AS likes FROM presets WHERE deleted_at IS NULL").first(),
    env.DB.prepare("SELECT COUNT(DISTINCT submitter_hash) AS n FROM presets WHERE submitter_hash<>''").first("n"),
    env.DB.prepare("SELECT COUNT(*) AS n FROM client_whitelist WHERE auto_approve=1").first("n"),
  ]);
  return json({
    ok: true,
    counts: { pending, approved, rejected, trash, clients, trusted, downloads: totals.downloads, likes: totals.likes },
  });
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------
const SORTS = {
  updated: "p.updated_at DESC",
  created: "p.created_at ASC",
  downloads: "p.downloads DESC, p.likes DESC",
  likes: "p.likes DESC, p.downloads DESC",
  expiry: "p.delete_after ASC",
};

// GET /api/admin/presets?status=&view=active|trash&q=&sort=&limit=
export async function listPresets(env, params) {
  const where = [];
  const binds = [];
  const view = params.get("view") || "active";
  // Trash lists deleted presets only; applied, cancelled and superseded owner
  // updates are also soft-deleted rows, but they are bookkeeping, not presets.
  where.push(view === "trash" ? "p.deleted_at IS NOT NULL AND p.update_of=''" : "p.deleted_at IS NULL");
  const status = params.get("status") || "";
  if (["pending", "approved", "rejected"].includes(status)) {
    where.push("p.status=?");
    binds.push(status);
  }
  const q = sanitizeText(params.get("q"), 120);
  if (q) {
    where.push("(p.title LIKE ? OR p.author_name LIKE ? OR p.id LIKE ? OR p.submitter_hash LIKE ?)");
    binds.push(`%${q}%`, `%${q}%`, `%${q}%`, `${q}%`);
  }
  const order = SORTS[params.get("sort")] || SORTS.updated;
  const { results } = await env.DB.prepare(
    `SELECT p.id,p.title,p.author_name,p.description,p.status,p.update_of,p.submitter_hash,p.downloads,p.likes,
            p.sha256,p.size_bytes,p.format_version,p.min_addon_version,p.created_at,p.updated_at,p.approved_at,p.rejected_at,
            p.deleted_at,p.delete_after,p.deleted_by,p.delete_reason,
            t.title AS target_title,
            (SELECT label FROM client_whitelist w WHERE w.submitter_hash=p.submitter_hash AND w.auto_approve=1) AS trusted_label
       FROM presets p LEFT JOIN presets t ON t.id=p.update_of
      WHERE ${where.join(" AND ")}
      ORDER BY ${order} LIMIT ?`,
  ).bind(...binds, limitOf(params.get("limit"))).all();
  return json({ ok: true, presets: (results || []).map(withFingerprint) });
}

// GET /api/admin/presets/:id: row, INI text, scan result, and (for updates) the live version.
export async function presetDetail(env, id) {
  const row = await env.DB.prepare(
    `SELECT p.*, t.title AS target_title,
            (SELECT label FROM client_whitelist w WHERE w.submitter_hash=p.submitter_hash AND w.auto_approve=1) AS trusted_label
       FROM presets p LEFT JOIN presets t ON t.id=p.update_of WHERE p.id=?`,
  ).bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const file = await env.PRESETS.get(row.r2_key);
  const iniText = file ? await file.text() : "";
  let current = null;
  if (row.update_of) {
    const target = await lifecycle.getPreset(env, row.update_of);
    const targetFile = target && (await env.PRESETS.get(target.r2_key));
    current = target ? { id: target.id, title: target.title, iniText: targetFile ? await targetFile.text() : "" } : null;
  }
  return json({
    ok: true,
    preset: withFingerprint(row),
    iniText,
    scan: iniText ? scanPresetIni(iniText) : { ok: false, errors: ["Preset file missing."], warnings: [] },
    current,
  });
}

export async function presetAction(request, env, actor, id, action) {
  const body = (await readJson(request)) || {};
  switch (action) {
    // sha256: the content the moderator reviewed; other content is not approved.
    case "approve": return result(await lifecycle.approve(env, actor, id, String(body.sha256 || "")));
    case "reject": return result(await lifecycle.reject(env, actor, id, body.note));
    case "delete": return result(await lifecycle.softDelete(env, actor, id, body.reason || "admin"));
    case "restore": return result(await lifecycle.restore(env, actor, id));
    case "purge": return result(await lifecycle.purge(env, actor, id));
    case "trust": return result(await lifecycle.trustSubmitterOf(env, actor, id, body.label, body.note));
    default: return bad("Unknown action.", 404);
  }
}

// POST /api/admin/presets/bulk { action, ids[], hashes{id: sha256} }: runs one action on many presets.
export async function bulkAction(request, env, actor) {
  const body = await readJson(request);
  const ids = Array.isArray(body?.ids) ? body.ids.map(String).slice(0, 200) : [];
  if (!ids.length) return bad("No presets selected.");
  const results = [];
  for (const id of ids) {
    const itemBody = { ...body, sha256: body.hashes?.[id] || "" };
    const res = await presetAction(new Request(request.url, { method: "POST", body: JSON.stringify(itemBody) }), env, actor, id, String(body.action || ""));
    results.push({ id, ...(await res.json()) });
  }
  return json({ ok: true, results });
}

// ---------------------------------------------------------------------------
// Trusted uploaders (auto-approve)
// ---------------------------------------------------------------------------
export async function listTrusted(env) {
  const { results } = await env.DB.prepare(
    `SELECT w.submitter_hash,w.label,w.note,w.created_at,w.updated_at,
            (SELECT COUNT(*) FROM presets p WHERE p.submitter_hash=w.submitter_hash AND p.update_of='' AND p.deleted_at IS NULL) AS presets
       FROM client_whitelist w WHERE w.auto_approve=1 ORDER BY w.updated_at DESC`,
  ).all();
  return json({ ok: true, trusted: (results || []).map(withFingerprint) });
}

// POST /api/admin/trusted { submitterHash | clientId, label, note }
export async function addTrusted(request, env, actor) {
  const body = await readJson(request);
  // A client id is turned into its hash with the public Worker's secret; without
  // that secret here, only hashes (e.g. from a preset's detail) can be used.
  if (!/^[a-f0-9]{64}$/i.test(body?.submitterHash || "") && body?.clientId && !env.DEVICE_HASH_SECRET) {
    return bad("Adding by client id is not set up on this admin page. Use the uploader hash, or trust them from one of their presets.");
  }
  const hash = /^[a-f0-9]{64}$/i.test(body?.submitterHash || "")
    ? body.submitterHash.toLowerCase()
    : body?.clientId ? await hashClientId(env, String(body.clientId)) : "";
  if (!hash) return bad("Provide a submitter hash or client id.");
  const now = nowIso();
  await env.DB.batch([
    env.DB.prepare(
      `INSERT INTO client_whitelist (submitter_hash,label,auto_approve,note,created_at,updated_at) VALUES (?,?,1,?,?,?)
       ON CONFLICT(submitter_hash) DO UPDATE SET label=excluded.label,auto_approve=1,note=excluded.note,updated_at=excluded.updated_at`,
    ).bind(hash, sanitizeText(body.label, 80), sanitizeText(body.note, 300), now, now),
    auditStatement(env, actor, "whitelist-add", "", clientFingerprint(hash)),
  ]);
  return json({ ok: true, submitterHash: hash, fingerprint: clientFingerprint(hash) });
}

export async function removeTrusted(env, actor, hash) {
  const removed = await env.DB.prepare("DELETE FROM client_whitelist WHERE submitter_hash=?").bind(hash.toLowerCase()).run();
  if (!removed.meta.changes) return bad("Trusted uploader not found.", 404);
  await audit(env, actor, "whitelist-delete", "", clientFingerprint(hash));
  return json({ ok: true });
}

// ---------------------------------------------------------------------------
// Audit log
// ---------------------------------------------------------------------------
export async function listAudit(env, params) {
  const { results } = await env.DB.prepare(
    "SELECT id,action,preset_id,admin_email_or_token AS actor,note,created_at FROM admin_audit ORDER BY id DESC LIMIT ?",
  ).bind(limitOf(params.get("limit"), 200, 1000)).all();
  return json({ ok: true, audit: results || [] });
}

// ---------------------------------------------------------------------------
// Addon releases (what the in-game updater offers)
// ---------------------------------------------------------------------------
export const getRelease = async (env) => json({ ok: true, release: await getUpdateSettings(env) }, 200, { "cache-control": "no-store" });

// PUT /api/admin/release { latestVersion, downloadPageUrl, changelog, publishedAt, critical }
export async function saveRelease(request, env, actor) {
  const body = await readJson(request);
  if (!body) return bad("Invalid JSON.");
  const latestVersion = normalizeVersion(body.latestVersion || body.version || "");
  if (!isVersion(latestVersion)) return bad("Version must look like 0.6.6.");
  const downloadPageUrl = String(body.downloadPageUrl || "").trim();
  if (!/^https:\/\/\S+$/i.test(downloadPageUrl) || downloadPageUrl.length > 300) return bad("Download URL must be an https URL.");
  const changelog = String(body.changelog || "").replace(/\r\n?/g, "\n").replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f\u007f]/g, "");
  if (!changelog.trim()) return bad("Changelog is required.");
  if (changelog.length > 50000) return bad("Changelog is too long.");

  const current = await getUpdateSettings(env);
  const values = {
    latestVersion,
    downloadPageUrl,
    changelog,
    publishedAt: String(body.publishedAt || "").trim().slice(0, 80),
    critical: body.critical ? "1" : "0",
  };
  // A different version must not keep offering the previous version's file.
  if (current.latestVersion !== latestVersion) Object.assign(values, { addonR2Key: "", addonSha256: "", addonSizeBytes: "0" });
  await saveUpdateSettings(env, values, actor);
  await audit(env, actor, "update-settings", "", `latest=${latestVersion}`);
  return getRelease(env);
}

// PUT /api/admin/release/artifact?version=: uploads the .addon64 for that version.
export async function uploadArtifact(request, env, actor) {
  const version = normalizeVersion(new URL(request.url).searchParams.get("version") || "");
  if (!isVersion(version)) return bad("Version must look like 0.6.6.");
  const bytes = await readBody(request, ARTIFACT_MAX_BYTES);
  if (!bytes) return bad("Addon file is too large.");
  if (!bytes.byteLength) return bad("Addon file is empty.");
  const sha256 = await sha256Hex(bytes);
  // A new key per file: the file players are offered is never overwritten. The
  // settings switch to it only after it is stored, and the file it replaces is
  // removed only after that (if it was an earlier upload of this same version).
  const key = `updates/${version}/CrimsonWeather-${sha256.slice(0, 16)}.addon64`;
  const previous = await getUpdateSettings(env);
  await env.PRESETS.put(key, bytes, { httpMetadata: { contentType: "application/octet-stream" } });
  // Not removed on failure: a previous upload of the same bytes may already be live under this key.
  await saveUpdateSettings(env, { latestVersion: version, addonR2Key: key, addonSha256: sha256, addonSizeBytes: String(bytes.byteLength) }, actor);
  await audit(env, actor, "update-artifact", "", `version=${version} sha256=${sha256}`);
  if (previous.addonR2Key !== key && previous.addonR2Key.startsWith(`updates/${version}/`)) await lifecycle.deleteFiles(env, previous.addonR2Key);
  return json({ ok: true, version, addonR2Key: key, addonSha256: sha256, addonSizeBytes: bytes.byteLength });
}

export async function rebuild(env, actor) {
  const catalog = await rebuildCatalog(env);
  await audit(env, actor, "catalog-rebuild", "", `presets=${catalog.presets.length}`);
  return json({ ok: true, presets: catalog.presets.length });
}
