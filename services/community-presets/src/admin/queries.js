import { likeTerm, normalizeLimit, sanitizeText } from "../shared/values.js";
import { withClientFingerprint } from "../auth/client-identity.js";
import { bad, json, text } from "../http/responses.js";
import { audit } from "../storage/audit-repository.js";
import { scanPresetIni } from "../presets/scanner.js";

export async function listSubmissions(env, status) {
  return listAdminPresets(env, new URLSearchParams({ status: status || "pending" }), "submissions");
}

export function newestPendingUpdateFilter(tableName = "presets") {
  return `NOT (
    ${tableName}.update_of<>'' AND EXISTS (
      SELECT 1 FROM presets newer
      WHERE newer.update_of=${tableName}.update_of
        AND newer.status='pending'
        AND newer.deleted_at IS NULL
        AND (
          newer.created_at > ${tableName}.created_at
          OR (newer.created_at = ${tableName}.created_at AND newer.id > ${tableName}.id)
        )
    )
  )`;
}

export function adminPresetWhere(searchParams, options = {}) {
  const where = [];
  const params = [];
  const deleted = searchParams.get("deleted") || options.deleted || "active";
  if (deleted === "trash" || deleted === "1" || deleted === "true") {
    where.push("deleted_at IS NOT NULL");
  } else if (deleted !== "all") {
    where.push("deleted_at IS NULL");
  }

  const status = searchParams.get("status") || options.status || "";
  if (["pending", "approved", "rejected"].includes(status)) {
    where.push("status=?");
    params.push(status);
    if (status === "pending") {
      where.push(newestPendingUpdateFilter("presets"));
    }
  }

  const rootOnly = searchParams.get("rootOnly") || options.rootOnly || "";
  if (rootOnly === "1" || rootOnly === "true") {
    where.push("update_of=''");
  }

  const client = sanitizeText(searchParams.get("client") || "", 80).toLowerCase();
  if (/^[a-f0-9]{12,64}$/.test(client)) {
    where.push("submitter_hash LIKE ?");
    params.push(`${client}%`);
  }

  const search = likeTerm(searchParams.get("q") || "");
  if (search) {
    where.push("(id LIKE ? OR title LIKE ? OR author_name LIKE ? OR description LIKE ? OR submitter_hash LIKE ?)");
    params.push(search, search, search, search, search);
  }

  return { where: where.length ? `WHERE ${where.join(" AND ")}` : "", params };
}

export function adminPresetOrder(sort) {
  switch (sort) {
    case "created": return "created_at DESC";
    case "oldest": return "created_at ASC";
    case "downloads": return "downloads DESC, updated_at DESC";
    case "likes": return "likes DESC, updated_at DESC";
    case "title": return "title COLLATE NOCASE ASC";
    case "delete_after": return "delete_after ASC, updated_at DESC";
    default: return "updated_at DESC";
  }
}

export async function listAdminPresets(env, searchParams, responseKey = "presets") {
  const { where, params } = adminPresetWhere(searchParams);
  const limit = normalizeLimit(searchParams.get("limit"), 200, 500);
  const order = adminPresetOrder(searchParams.get("sort") || "");
  const { results } = await env.DB.prepare(
    `SELECT * FROM presets ${where} ORDER BY ${order} LIMIT ?`
  ).bind(...params, limit).all();
  const rows = (results || []).map(withClientFingerprint);
  return json({ ok: true, [responseKey]: rows });
}

export async function scalarCount(env, sql, ...params) {
  const row = await env.DB.prepare(sql).bind(...params).first();
  return Number(row?.count || 0);
}

export async function adminOverview(env) {
  const [
    pending,
    approved,
    rejected,
    deleted,
    clients,
    totals,
    topPresets,
    newestPending
  ] = await Promise.all([
    scalarCount(env, "SELECT COUNT(*) AS count FROM presets WHERE status='pending' AND deleted_at IS NULL"),
    scalarCount(env, "SELECT COUNT(*) AS count FROM presets WHERE status='approved' AND update_of='' AND deleted_at IS NULL"),
    scalarCount(env, "SELECT COUNT(*) AS count FROM presets WHERE status='rejected' AND deleted_at IS NULL"),
    scalarCount(env, "SELECT COUNT(*) AS count FROM presets WHERE deleted_at IS NOT NULL"),
    scalarCount(env, "SELECT COUNT(DISTINCT submitter_hash) AS count FROM presets WHERE submitter_hash<>''"),
    env.DB.prepare("SELECT COALESCE(SUM(downloads),0) AS downloads, COALESCE(SUM(likes),0) AS likes, COUNT(*) AS presets FROM presets WHERE deleted_at IS NULL").first(),
    env.DB.prepare("SELECT id,title,author_name,downloads,likes,status,updated_at FROM presets WHERE update_of='' AND deleted_at IS NULL ORDER BY downloads DESC, likes DESC, updated_at DESC LIMIT 8").all(),
    env.DB.prepare(`SELECT id,title,author_name,created_at,updated_at,submitter_hash,update_of FROM presets WHERE status='pending' AND deleted_at IS NULL AND ${newestPendingUpdateFilter("presets")} ORDER BY created_at ASC LIMIT 8`).all()
  ]);
  return json({
    ok: true,
    counts: {
      pending,
      approved,
      rejected,
      deleted,
      clients,
      presets: Number(totals?.presets || 0),
      downloads: Number(totals?.downloads || 0),
      likes: Number(totals?.likes || 0)
    },
    topPresets: topPresets.results || [],
    newestPending: (newestPending.results || []).map(withClientFingerprint)
  });
}

export async function listAdminClients(env, searchParams) {
  const where = ["p.submitter_hash<>''"];
  const params = [];
  const search = likeTerm(searchParams.get("q") || "");
  if (search) {
    where.push("(p.submitter_hash LIKE ? OR p.title LIKE ? OR p.author_name LIKE ? OR w.label LIKE ? OR w.note LIKE ?)");
    params.push(search, search, search, search, search);
  }
  const orderName = searchParams.get("sort") || "";
  const order = orderName === "uploads" ? "upload_count DESC, last_upload DESC"
    : orderName === "downloads" ? "total_downloads DESC, last_upload DESC"
    : orderName === "likes" ? "total_likes DESC, last_upload DESC"
    : "last_upload DESC";
  const limit = normalizeLimit(searchParams.get("limit"), 200, 500);
  const { results } = await env.DB.prepare(
    `SELECT
       p.submitter_hash,
       COUNT(*) AS upload_count,
       SUM(CASE WHEN p.status='pending' AND p.deleted_at IS NULL THEN 1 ELSE 0 END) AS pending_count,
       SUM(CASE WHEN p.status='approved' AND p.update_of='' AND p.deleted_at IS NULL THEN 1 ELSE 0 END) AS approved_count,
       SUM(CASE WHEN p.status='rejected' AND p.deleted_at IS NULL THEN 1 ELSE 0 END) AS rejected_count,
       SUM(CASE WHEN p.deleted_at IS NOT NULL THEN 1 ELSE 0 END) AS deleted_count,
       COALESCE(SUM(p.downloads),0) AS total_downloads,
       COALESCE(SUM(p.likes),0) AS total_likes,
       MIN(p.created_at) AS first_upload,
       MAX(p.updated_at) AS last_upload,
       COALESCE(w.label,'') AS label,
       COALESCE(w.note,'') AS note,
       COALESCE(w.auto_approve,0) AS auto_approve,
       w.updated_at AS whitelist_updated_at
     FROM presets p
     LEFT JOIN client_whitelist w ON w.submitter_hash=p.submitter_hash
     WHERE ${where.join(" AND ")}
     GROUP BY p.submitter_hash
     ORDER BY ${order}
     LIMIT ?`
  ).bind(...params, limit).all();
  return json({ ok: true, clients: (results || []).map(withClientFingerprint) });
}

export async function listAdminAudit(env, searchParams) {
  const limit = normalizeLimit(searchParams.get("limit"), 100, 300);
  const { results } = await env.DB.prepare("SELECT * FROM admin_audit ORDER BY created_at DESC LIMIT ?").bind(limit).all();
  return json({ ok: true, audit: results || [] });
}

export async function adminPresetDetail(env, id) {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=?").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const object = await env.PRESETS.get(row.r2_key);
  const iniText = object ? await object.text() : "";
  return json({
    ok: true,
    preset: withClientFingerprint(row),
    iniText,
    scan: iniText ? scanPresetIni(iniText) : { ok: false, errors: ["Preset file missing."], warnings: [] }
  });
}

export async function adminPresetIni(env, id) {
  const row = await env.DB.prepare("SELECT id,r2_key FROM presets WHERE id=?").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const object = await env.PRESETS.get(row.r2_key);
  if (!object) return bad("Preset file missing.", 404);
  return new Response(object.body, {
    headers: {
      "content-type": "text/plain; charset=utf-8",
      "content-disposition": `attachment; filename="${id}.ini"`
    }
  });
}
