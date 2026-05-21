const PRESET_HEADER = "[CrimsonWeatherPreset]";
const CURRENT_FORMAT_VERSION = 6;
const CATALOG_KEY = "catalog/catalog.v1.json";
const ALLOWED_SECTIONS = new Set([
  "Meta", "Weather", "Time", "Cloud", "Experiment", "Celestial", "Atmosphere"
]);
const ALLOWED_KEYS = new Set([
  "FormatVersion", "Enabled",
  "ForceClearSky", "NoRain", "Rain", "Thunder", "NoDust", "Dust", "NoSnow", "Snow",
  "SnowAccumBoundaryAEnabled", "SnowAccumBoundaryA", "SnowAccumBoundaryBEnabled", "SnowAccumBoundaryB",
  "SnowCoverageThresholdEnabled", "SnowCoverageThreshold",
  "VisualTimeOverride", "ProgressVisualTime", "ProgressVisualTimeMatchGameTime", "ProgressVisualTimeIntervalMs", "TimeHour",
  "CloudAmountEnabled", "CloudAmount", "CloudHeightEnabled", "CloudHeight",
  "CloudDensityEnabled", "CloudDensity", "MidCloudsEnabled", "MidClouds",
  "HighCloudLayerEnabled", "HighCloudLayer", "CloudAlphaEnabled", "CloudAlpha",
  "CloudFadeRangeEnabled", "CloudFadeRange", "CloudDetailRatioEnabled", "CloudDetailRatio",
  "CloudPhaseFrontEnabled", "CloudPhaseFront", "CloudScatteringCoefficientEnabled",
  "CloudScatteringCoefficient", "CloudFlowEnabled", "CloudFlow", "CloudVisibleRangeEnabled",
  "CloudVisibleRange", "RayleighHeightEnabled",
  "RayleighHeight", "OzoneRatioEnabled", "OzoneRatio", "RayleighScatteringColorEnabled",
  "RayleighScatteringColorR", "RayleighScatteringColorG", "RayleighScatteringColorB",
  "2CEnabled", "2C", "2DEnabled", "2D", "CloudVariationEnabled", "CloudVariation",
  "PuddleScaleEnabled", "PuddleScale",
  "NightSkyTiltEnabled", "NightSkyTilt", "NightSkyPhaseEnabled", "NightSkyPhase",
  "SunSizeEnabled", "SunSize", "SunLightIntensityEnabled", "SunLightIntensity",
  "SunYawEnabled", "SunYaw", "SunPitchEnabled", "SunPitch", "MoonSizeEnabled",
  "MoonSize", "MoonLightIntensityEnabled", "MoonLightIntensity", "MoonYawEnabled",
  "MoonYaw", "MoonPitchEnabled", "MoonPitch", "MoonRollEnabled", "MoonRoll",
  "MoonTextureEnabled", "MoonTexture", "MilkywayTextureEnabled", "MilkywayTexture",
  "FogEnabled", "Fog", "NativeFogEnabled", "NativeFog", "VolumeFogScatterColorEnabled",
  "VolumeFogScatterColorR", "VolumeFogScatterColorG", "VolumeFogScatterColorB",
  "VolumeFogScatterColorA", "MieScatterColorEnabled", "MieScatterColorR",
  "MieScatterColorG", "MieScatterColorB", "MieScatterColorA", "MieScaleHeightEnabled",
  "MieScaleHeight", "MieAerosolDensityEnabled", "MieAerosolDensity",
  "MieAerosolAbsorptionEnabled", "MieAerosolAbsorption", "HeightFogBaselineEnabled",
  "HeightFogBaseline", "HeightFogFalloffEnabled", "HeightFogFalloff", "NoFog", "Wind", "NoWind"
]);
const STRING_KEYS = new Set(["MoonTexture", "MilkywayTexture"]);

function json(body, status = 200, headers = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", ...headers }
  });
}

function text(body, status = 200, headers = {}) {
  return new Response(body, { status, headers: { "content-type": "text/plain; charset=utf-8", ...headers } });
}

function nowIso() {
  return new Date().toISOString();
}

function bad(message, status = 400, details = undefined) {
  return json({ ok: false, error: message, details }, status);
}

function sanitizeText(value, maxLen) {
  return String(value ?? "").replace(/[\u0000-\u001f\u007f]/g, " ").replace(/\s+/g, " ").trim().slice(0, maxLen);
}

function slugify(value) {
  return sanitizeText(value, 80).toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 48) || "preset";
}

function bytesOf(textValue) {
  return new TextEncoder().encode(textValue);
}

async function sha256Hex(data) {
  const bytes = typeof data === "string" ? bytesOf(data) : data;
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function hmacHex(secret, value) {
  if (!value) return "";
  const key = await crypto.subtle.importKey("raw", bytesOf(secret || "dev-secret"), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const sig = await crypto.subtle.sign("HMAC", key, bytesOf(value));
  return [...new Uint8Array(sig)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function deviceHashSecret(env) {
  return env.DEVICE_HASH_SECRET || env.ADMIN_TOKEN || "dev-secret";
}

async function hashClientId(env, clientId) {
  return hmacHex(deviceHashSecret(env), clientId);
}

function clientFingerprint(submitterHash) {
  return submitterHash ? `${submitterHash.slice(0, 12)}...${submitterHash.slice(-6)}` : "";
}

async function submitterHashFromRequest(request, env) {
  const clientId = request.headers.get("x-cw-client-id") || "";
  if (!clientId || clientId.length > 128) return "";
  return hashClientId(env, clientId);
}

async function whitelistRow(env, submitterHash) {
  if (!submitterHash) return null;
  return env.DB.prepare("SELECT * FROM client_whitelist WHERE submitter_hash=? AND auto_approve=1").bind(submitterHash).first();
}

function extractFormatVersion(lines) {
  for (const line of lines) {
    const eq = line.indexOf("=");
    if (eq < 0) continue;
    if (line.slice(0, eq).trim().toLowerCase() === "formatversion") {
      const parsed = Number.parseInt(line.slice(eq + 1).trim(), 10);
      return Number.isFinite(parsed) ? parsed : 0;
    }
  }
  return 0;
}

export function scanPresetIni(iniText, maxBytes = 65536) {
  const result = { ok: true, errors: [], warnings: [], formatVersion: 0 };
  if (typeof iniText !== "string" || !iniText.trim()) {
    result.ok = false;
    result.errors.push("Preset text is empty.");
    return result;
  }
  const byteLength = bytesOf(iniText).byteLength;
  if (byteLength > maxBytes) result.errors.push(`Preset exceeds ${maxBytes} bytes.`);
  if (/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/.test(iniText)) result.errors.push("Preset contains control characters.");
  const lines = iniText.replace(/^\uFEFF/, "").split(/\r?\n/).map((line) => line.trim());
  if (lines[0] !== PRESET_HEADER) result.errors.push("Missing [CrimsonWeatherPreset] header.");
  result.formatVersion = extractFormatVersion(lines);
  if (result.formatVersion > CURRENT_FORMAT_VERSION) {
    result.errors.push(`FormatVersion ${result.formatVersion} is newer than supported ${CURRENT_FORMAT_VERSION}.`);
  }

  let headerSeen = false;
  for (const raw of lines) {
    if (!raw || raw.startsWith(";") || raw.startsWith("#")) continue;
    if (raw === PRESET_HEADER) {
      headerSeen = true;
      continue;
    }
    if (!headerSeen) continue;
    if (raw.startsWith("[") && raw.endsWith("]")) {
      const section = raw.slice(1, -1);
      const normalized = section.startsWith("Region.") ? section.split(".").slice(-1)[0] : section;
      if (!ALLOWED_SECTIONS.has(normalized) && !/^Region\.[A-Za-z0-9_ -]+$/.test(section)) {
        result.errors.push(`Unknown section: ${section}`);
      }
      continue;
    }
    const eq = raw.indexOf("=");
    if (eq < 0) {
      result.errors.push(`Invalid line: ${raw.slice(0, 60)}`);
      continue;
    }
    const key = raw.slice(0, eq).trim();
    const value = raw.slice(eq + 1).trim();
    if (!ALLOWED_KEYS.has(key)) result.errors.push(`Unknown key: ${key}`);
    if (/https?:\/\//i.test(value) || /(^|[\\\/])\.\.([\\\/]|$)/.test(value) || /^[a-z]:[\\\/]/i.test(value) || /^\\\\/.test(value)) {
      result.errors.push(`Unsafe value for ${key}.`);
    }
    if (!STRING_KEYS.has(key) && key !== "Enabled") {
      const lowered = value.toLowerCase();
      const isBool = ["0", "1", "true", "false", "yes", "no", "on", "off"].includes(lowered);
      if (!isBool && value !== "") {
        const parsed = Number(value);
        if (!Number.isFinite(parsed)) result.errors.push(`Non-finite numeric value for ${key}.`);
      }
    }
  }
  result.ok = result.errors.length === 0;
  return result;
}

function cookieValue(request, name) {
  const cookie = request.headers.get("cookie") || "";
  for (const part of cookie.split(";")) {
    const [rawKey, ...rawValue] = part.trim().split("=");
    if (rawKey === name) return rawValue.join("=");
  }
  return "";
}

function base64UrlEncode(value) {
  return btoa(value).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

function base64UrlDecode(value) {
  const padded = value.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((value.length + 3) % 4);
  return atob(padded);
}

async function makeAdminSession(env) {
  const payload = base64UrlEncode(JSON.stringify({ exp: Date.now() + 24 * 60 * 60 * 1000 }));
  const sig = await hmacHex(env.ADMIN_TOKEN, payload);
  return `${payload}.${sig}`;
}

async function hasValidAdminSession(request, env) {
  if (!env.ADMIN_TOKEN) return false;
  const session = cookieValue(request, "cw_admin_session");
  const [payload, sig] = session.split(".");
  if (!payload || !sig) return false;
  const expected = await hmacHex(env.ADMIN_TOKEN, payload);
  if (sig !== expected) return false;
  try {
    const data = JSON.parse(base64UrlDecode(payload));
    return Number(data.exp || 0) > Date.now();
  } catch {
    return false;
  }
}

async function isAdmin(request, env) {
  const auth = request.headers.get("authorization") || "";
  const token = env.ADMIN_TOKEN || "";
  if (token && auth === `Bearer ${token}`) return true;
  if (request.headers.get("cf-access-authenticated-user-email")) return true;
  return hasValidAdminSession(request, env);
}

function adminIdentity(request, env) {
  if (env.ADMIN_TOKEN && (request.headers.get("authorization") || "") === `Bearer ${env.ADMIN_TOKEN}`) return "admin-token";
  return request.headers.get("cf-access-authenticated-user-email") || "admin-session";
}

async function readJson(request) {
  try {
    const length = Number(request.headers.get("content-length") || 0);
    if (length > 140000) return null;
    return await request.json();
  } catch {
    return null;
  }
}

async function queryApprovedPresets(env) {
  const { results } = await env.DB.prepare(
    "SELECT id,title,author_name,description,tags_json,sha256,size_bytes,format_version,min_addon_version,downloads,likes,created_at,updated_at,approved_at FROM presets WHERE status='approved' AND update_of='' ORDER BY updated_at DESC"
  ).all();
  return results || [];
}

function catalogFromRows(rows, generatedAt = nowIso()) {
  return {
    schemaVersion: 1,
    generatedAt,
    presets: rows.map((row) => ({
      id: row.id,
      title: row.title,
      author: row.author_name,
      description: row.description || "",
      tags: JSON.parse(row.tags_json || "[]"),
      formatVersion: row.format_version,
      minAddonVersion: row.min_addon_version,
      publishedAt: row.approved_at || row.created_at,
      updatedAt: row.updated_at,
      downloads: row.downloads || 0,
      likes: row.likes || 0,
      file: {
        url: `/api/v1/presets/${row.id}/download`,
        sha256: row.sha256,
        size: row.size_bytes
      },
      safety: {
        approved: true
      }
    }))
  };
}

async function rebuildCatalog(env) {
  const catalog = catalogFromRows(await queryApprovedPresets(env));
  await env.PRESETS.put(CATALOG_KEY, JSON.stringify(catalog, null, 2), {
    httpMetadata: { contentType: "application/json; charset=utf-8" }
  });
  return catalog;
}

async function getCatalog(env) {
  const cached = await env.PRESETS.get(CATALOG_KEY);
  if (cached) {
    return new Response(cached.body, {
      headers: {
        "content-type": "application/json; charset=utf-8",
        "cache-control": `public, max-age=${Number(env.CATALOG_CACHE_SECONDS || 300)}`
      }
    });
  }
  return json(await rebuildCatalog(env), 200, { "cache-control": "public, max-age=60" });
}

async function submitPreset(request, env) {
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
  const r2Key = `pending/${id}/preset.ini`;
  const created = nowIso();
  const submitterHash = await hashClientId(env, clientId);
  const trusted = await whitelistRow(env, submitterHash);
  const status = trusted ? "approved" : "pending";
  const r2KeyFinal = `${status}/${id}/preset.ini`;
  await env.PRESETS.put(r2Key, bytes, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
  if (trusted) {
    await env.PRESETS.put(r2KeyFinal, bytes, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
  }
  await env.DB.prepare(
    "INSERT INTO presets (id,title,author_name,description,tags_json,status,r2_key,sha256,size_bytes,format_version,min_addon_version,submitter_hash,safety_status,safety_summary,created_at,updated_at,approved_at) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
  ).bind(
    id, title, author, description, JSON.stringify(tags), status, trusted ? r2KeyFinal : r2Key, hash, bytes.byteLength,
    scan.formatVersion || 0, body.clientVersion || env.MIN_ADDON_VERSION || "0.6.3",
    submitterHash, "passed", trusted ? `Auto-approved whitelist ${clientFingerprint(submitterHash)}` : scan.warnings.join("; "), created, created,
    trusted ? created : null
  ).run();
  if (trusted) await rebuildCatalog(env);
  return json({ ok: true, id, status, autoApproved: Boolean(trusted), scan });
}

async function downloadPreset(request, env, id) {
  const row = await env.DB.prepare("SELECT id,r2_key,status FROM presets WHERE id=? AND status='approved'").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const object = await env.PRESETS.get(row.r2_key);
  if (!object) return bad("Preset file missing.", 404);
  const clientId = request.headers.get("x-cw-client-id") || "";
  if (clientId) {
    const deviceHash = await hashClientId(env, clientId);
    const day = new Date().toISOString().slice(0, 10);
    const existing = await env.DB.prepare(
      "SELECT 1 AS found FROM preset_downloads_daily WHERE preset_id=? AND device_hash=? AND day=?"
    ).bind(id, deviceHash, day).first();
    if (!existing) {
      await env.DB.prepare("INSERT INTO preset_downloads_daily (preset_id,device_hash,day,created_at) VALUES (?,?,?,?)")
        .bind(id, deviceHash, day, nowIso()).run();
      await env.DB.prepare("UPDATE presets SET downloads=downloads+1, updated_at=? WHERE id=?").bind(nowIso(), id).run();
      await rebuildCatalog(env);
    }
  }
  return new Response(object.body, {
    headers: {
      "content-type": "text/plain; charset=utf-8",
      "content-disposition": `attachment; filename="${id}.ini"`
    }
  });
}

async function toggleLike(request, env, id) {
  const row = await env.DB.prepare("SELECT id FROM presets WHERE id=? AND status='approved'").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const clientId = request.headers.get("x-cw-client-id") || "";
  if (!clientId) return bad("Missing anonymous client id.", 400);
  const deviceHash = await hashClientId(env, clientId);
  const existing = await env.DB.prepare("SELECT 1 FROM preset_likes WHERE preset_id=? AND device_hash=?").bind(id, deviceHash).first();
  let liked = false;
  if (existing) {
    await env.DB.prepare("DELETE FROM preset_likes WHERE preset_id=? AND device_hash=?").bind(id, deviceHash).run();
    await env.DB.prepare("UPDATE presets SET likes=MAX(0, likes-1), updated_at=? WHERE id=?").bind(nowIso(), id).run();
  } else {
    await env.DB.prepare("INSERT INTO preset_likes (preset_id,device_hash,created_at) VALUES (?,?,?)").bind(id, deviceHash, nowIso()).run();
    await env.DB.prepare("UPDATE presets SET likes=likes+1, updated_at=? WHERE id=?").bind(nowIso(), id).run();
    liked = true;
  }
  await rebuildCatalog(env);
  const updated = await env.DB.prepare("SELECT likes FROM presets WHERE id=?").bind(id).first();
  return json({ ok: true, liked, likes: updated?.likes || 0 });
}

async function listSubmissions(env, status) {
  const allowed = ["all", "pending", "approved", "rejected"].includes(status) ? status : "pending";
  let stmt;
  if (allowed === "all") {
    stmt = env.DB.prepare("SELECT * FROM presets ORDER BY updated_at DESC LIMIT 300");
  } else if (allowed === "approved") {
    stmt = env.DB.prepare("SELECT * FROM presets WHERE status='approved' AND update_of='' ORDER BY created_at DESC LIMIT 300");
  } else {
    stmt = env.DB.prepare("SELECT * FROM presets WHERE status=? ORDER BY created_at DESC LIMIT 300").bind(allowed);
  }
  const { results } = await stmt.all();
  return json({ ok: true, submissions: results || [] });
}

async function audit(env, request, action, id, note = "") {
  await env.DB.prepare("INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) VALUES (?,?,?,?,?)")
    .bind(action, id || "", adminIdentity(request, env), note, nowIso()).run();
}

async function approveSubmission(request, env, id) {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND status='pending'").bind(id).first();
  if (!row) return bad("Pending submission not found.", 404);
  const object = await env.PRESETS.get(row.r2_key);
  if (!object) return bad("Pending preset file missing.", 404);
  if (row.update_of) {
    const target = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND update_of=''").bind(row.update_of).first();
    if (!target) return bad("Target preset for update was not found.", 404);
    const approvedKey = `approved/${target.id}/preset.ini`;
    await env.PRESETS.put(approvedKey, object.body, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
    const approvedAt = target.approved_at || nowIso();
    await env.DB.prepare(
      "UPDATE presets SET title=?,author_name=?,description=?,tags_json=?,status='approved',r2_key=?,sha256=?,size_bytes=?,format_version=?,min_addon_version=?,safety_status=?,safety_summary=?,updated_at=?,approved_at=? WHERE id=?"
    ).bind(
      row.title, row.author_name, row.description, row.tags_json, approvedKey, row.sha256, row.size_bytes,
      row.format_version, row.min_addon_version, row.safety_status, row.safety_summary, nowIso(), approvedAt, target.id
    ).run();
    await env.DB.prepare("DELETE FROM presets WHERE id=?").bind(id).run();
    if (row.r2_key) await env.PRESETS.delete(row.r2_key);
    await audit(env, request, "approve-update", target.id, id);
    await rebuildCatalog(env);
    return json({ ok: true, id: target.id, updateId: id, status: "approved" });
  }
  const approvedKey = `approved/${id}/preset.ini`;
  await env.PRESETS.put(approvedKey, object.body, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
  await env.DB.prepare("UPDATE presets SET status='approved', r2_key=?, approved_at=?, updated_at=? WHERE id=?")
    .bind(approvedKey, nowIso(), nowIso(), id).run();
  await audit(env, request, "approve", id);
  await rebuildCatalog(env);
  return json({ ok: true, id, status: "approved" });
}

async function rejectSubmission(request, env, id) {
  const body = await readJson(request) || {};
  const row = await env.DB.prepare("SELECT id FROM presets WHERE id=? AND status='pending'").bind(id).first();
  if (!row) return bad("Pending submission not found.", 404);
  await env.DB.prepare("UPDATE presets SET status='rejected', rejected_at=?, updated_at=? WHERE id=?").bind(nowIso(), nowIso(), id).run();
  await audit(env, request, "reject", id, sanitizeText(body.note, 300));
  return json({ ok: true, id, status: "rejected" });
}

async function batchSubmissions(request, env) {
  const body = await readJson(request);
  const approvals = Array.isArray(body?.approve) ? body.approve : [];
  const rejections = Array.isArray(body?.reject) ? body.reject : [];
  const results = [];
  for (const id of approvals) results.push(await (await approveSubmission(request, env, String(id))).json());
  for (const id of rejections) results.push(await (await rejectSubmission(request, env, String(id))).json());
  return json({ ok: true, results });
}

async function adminPresetDetail(env, id) {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=?").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const object = await env.PRESETS.get(row.r2_key);
  const iniText = object ? await object.text() : "";
  return json({
    ok: true,
    preset: row,
    iniText,
    scan: iniText ? scanPresetIni(iniText) : { ok: false, errors: ["Preset file missing."], warnings: [] }
  });
}

async function adminPresetIni(env, id) {
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

async function deletePresetAdmin(request, env, id) {
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=?").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const keys = new Set([
    row.r2_key,
    `pending/${id}/preset.ini`,
    `approved/${id}/preset.ini`
  ]);
  for (const key of keys) {
    if (key) await env.PRESETS.delete(key);
  }
  await env.DB.prepare("DELETE FROM preset_likes WHERE preset_id=?").bind(id).run();
  await env.DB.prepare("DELETE FROM preset_downloads_daily WHERE preset_id=?").bind(id).run();
  await audit(env, request, "delete", id, row.status || "");
  await env.DB.prepare("DELETE FROM presets WHERE id=?").bind(id).run();
  if (row.status === "approved") await rebuildCatalog(env);
  return json({ ok: true, id, deleted: true });
}

async function deletePresetFiles(env, id, row) {
  const keys = new Set([
    row?.r2_key,
    `pending/${id}/preset.ini`,
    `approved/${id}/preset.ini`
  ]);
  for (const key of keys) {
    if (key) await env.PRESETS.delete(key);
  }
}

async function listMyPresets(request, env) {
  const submitterHash = await submitterHashFromRequest(request, env);
  if (!submitterHash) return bad("Missing anonymous client id.", 400);
  const { results } = await env.DB.prepare(
    "SELECT id,title,author_name,description,tags_json,status,update_of,downloads,likes,created_at,updated_at,approved_at,rejected_at FROM presets WHERE submitter_hash=? AND update_of='' ORDER BY updated_at DESC LIMIT 100"
  ).bind(submitterHash).all();
  return json({ ok: true, clientFingerprint: clientFingerprint(submitterHash), presets: results || [] });
}

async function deleteMyPreset(request, env, id) {
  const submitterHash = await submitterHashFromRequest(request, env);
  if (!submitterHash) return bad("Missing anonymous client id.", 400);
  const row = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND submitter_hash=?").bind(id, submitterHash).first();
  if (!row) return bad("Preset not found for this client.", 404);
  await deletePresetFiles(env, id, row);
  await env.DB.prepare("DELETE FROM preset_likes WHERE preset_id=?").bind(id).run();
  await env.DB.prepare("DELETE FROM preset_downloads_daily WHERE preset_id=?").bind(id).run();
  await env.DB.prepare("DELETE FROM presets WHERE id=?").bind(id).run();
  if (row.status === "approved" && !row.update_of) await rebuildCatalog(env);
  return json({ ok: true, id, deleted: true });
}

async function updateMyPreset(request, env, id) {
  const submitterHash = await submitterHashFromRequest(request, env);
  if (!submitterHash) return bad("Missing anonymous client id.", 400);
  const target = await env.DB.prepare("SELECT * FROM presets WHERE id=? AND submitter_hash=? AND update_of=''").bind(id, submitterHash).first();
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
    const r2Key = target.status === "approved" ? `approved/${id}/preset.ini` : target.r2_key;
    await env.PRESETS.put(r2Key, bytes, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
    await env.DB.prepare(
      "UPDATE presets SET title=?,author_name=?,description=?,tags_json=?,r2_key=?,sha256=?,size_bytes=?,format_version=?,min_addon_version=?,safety_status=?,safety_summary=?,updated_at=?,approved_at=COALESCE(approved_at, ?) WHERE id=?"
    ).bind(
      title, author, description, JSON.stringify(tags), r2Key, hash, bytes.byteLength,
      scan.formatVersion || 0, body.clientVersion || env.MIN_ADDON_VERSION || "0.6.3",
      "passed", trusted ? `Auto-approved whitelist ${clientFingerprint(submitterHash)}` : scan.warnings.join("; "),
      updated, target.status === "approved" ? updated : null, id
    ).run();
    if (target.status === "approved") await rebuildCatalog(env);
    return json({ ok: true, id, status: target.status, autoApproved: Boolean(trusted), scan });
  }

  const updateId = `${slugify(title)}-update-${crypto.randomUUID().slice(0, 8)}`;
  const r2Key = `pending/${updateId}/preset.ini`;
  await env.PRESETS.put(r2Key, bytes, { httpMetadata: { contentType: "text/plain; charset=utf-8" } });
  await env.DB.prepare(
    "INSERT INTO presets (id,title,author_name,description,tags_json,status,r2_key,sha256,size_bytes,format_version,min_addon_version,submitter_hash,safety_status,safety_summary,created_at,updated_at,update_of) VALUES (?,?,?,?,?,'pending',?,?,?,?,?,?,?,?,?,?,?)"
  ).bind(
    updateId, title, author, description, JSON.stringify(tags), r2Key, hash, bytes.byteLength,
    scan.formatVersion || 0, body.clientVersion || env.MIN_ADDON_VERSION || "0.6.3",
    submitterHash, "passed", scan.warnings.join("; "), updated, updated, id
  ).run();
  return json({ ok: true, id, updateId, status: "pending", autoApproved: false, scan });
}

async function listWhitelist(env) {
  const { results } = await env.DB.prepare("SELECT * FROM client_whitelist ORDER BY updated_at DESC LIMIT 200").all();
  return json({ ok: true, clients: results || [] });
}

async function addWhitelist(request, env) {
  const body = await readJson(request);
  if (!body) return bad("Invalid JSON.");
  const rawClientId = sanitizeText(body.clientId, 160);
  const providedHash = String(body.submitterHash || "").trim().toLowerCase();
  let submitterHash = "";
  if (rawClientId) {
    submitterHash = await hashClientId(env, rawClientId);
  } else if (/^[a-f0-9]{64}$/.test(providedHash)) {
    submitterHash = providedHash;
  }
  if (!submitterHash) return bad("Provide a raw ClientId or a 64-character submitter hash.");
  const label = sanitizeText(body.label, 80);
  const note = sanitizeText(body.note, 300);
  const autoApprove = body.autoApprove === false ? 0 : 1;
  const now = nowIso();
  await env.DB.prepare(
    "INSERT INTO client_whitelist (submitter_hash,label,auto_approve,note,created_at,updated_at) VALUES (?,?,?,?,?,?) ON CONFLICT(submitter_hash) DO UPDATE SET label=excluded.label,auto_approve=excluded.auto_approve,note=excluded.note,updated_at=excluded.updated_at"
  ).bind(submitterHash, label, autoApprove, note, now, now).run();
  await audit(env, request, "whitelist-add", "", clientFingerprint(submitterHash));
  return json({ ok: true, submitterHash, fingerprint: clientFingerprint(submitterHash), autoApprove: Boolean(autoApprove) });
}

async function whitelistFromPreset(request, env, id) {
  const body = await readJson(request) || {};
  const row = await env.DB.prepare("SELECT id,title,author_name,submitter_hash FROM presets WHERE id=?").bind(id).first();
  if (!row || !row.submitter_hash) return bad("Preset submitter was not found.", 404);
  const now = nowIso();
  const label = sanitizeText(body.label, 80) || row.author_name || row.title || id;
  const note = sanitizeText(body.note, 300);
  await env.DB.prepare(
    "INSERT INTO client_whitelist (submitter_hash,label,auto_approve,note,created_at,updated_at) VALUES (?,?,?,?,?,?) ON CONFLICT(submitter_hash) DO UPDATE SET label=excluded.label,auto_approve=excluded.auto_approve,note=excluded.note,updated_at=excluded.updated_at"
  ).bind(row.submitter_hash, label, 1, note, now, now).run();
  await audit(env, request, "whitelist-from-preset", id, clientFingerprint(row.submitter_hash));
  return json({ ok: true, submitterHash: row.submitter_hash, fingerprint: clientFingerprint(row.submitter_hash), label });
}

async function deleteWhitelist(request, env, submitterHash) {
  if (!/^[a-f0-9]{64}$/i.test(submitterHash)) return bad("Invalid submitter hash.", 400);
  await env.DB.prepare("DELETE FROM client_whitelist WHERE submitter_hash=?").bind(submitterHash.toLowerCase()).run();
  await audit(env, request, "whitelist-delete", "", clientFingerprint(submitterHash));
  return json({ ok: true, deleted: true });
}

function crc32(bytes) {
  let c = ~0;
  for (const b of bytes) {
    c ^= b;
    for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1));
  }
  return (~c) >>> 0;
}

function u16(n) { return [n & 255, (n >>> 8) & 255]; }
function u32(n) { return [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255]; }

function zipStore(files) {
  const chunks = [];
  const central = [];
  let offset = 0;
  for (const file of files) {
    const name = bytesOf(file.name);
    const data = typeof file.data === "string" ? bytesOf(file.data) : file.data;
    const crc = crc32(data);
    const local = new Uint8Array([
      ...u32(0x04034b50), ...u16(20), ...u16(0), ...u16(0), ...u16(0), ...u16(0),
      ...u32(crc), ...u32(data.length), ...u32(data.length), ...u16(name.length), ...u16(0)
    ]);
    chunks.push(local, name, data);
    central.push({ name, crc, size: data.length, offset });
    offset += local.length + name.length + data.length;
  }
  const centralStart = offset;
  for (const entry of central) {
    const header = new Uint8Array([
      ...u32(0x02014b50), ...u16(20), ...u16(20), ...u16(0), ...u16(0), ...u16(0), ...u16(0),
      ...u32(entry.crc), ...u32(entry.size), ...u32(entry.size), ...u16(entry.name.length),
      ...u16(0), ...u16(0), ...u16(0), ...u16(0), ...u32(0), ...u32(entry.offset)
    ]);
    chunks.push(header, entry.name);
    offset += header.length + entry.name.length;
  }
  const centralSize = offset - centralStart;
  chunks.push(new Uint8Array([
    ...u32(0x06054b50), ...u16(0), ...u16(0), ...u16(central.length), ...u16(central.length),
    ...u32(centralSize), ...u32(centralStart), ...u16(0)
  ]));
  return new Blob(chunks, { type: "application/zip" });
}

async function exportPending(env) {
  const { results } = await env.DB.prepare("SELECT * FROM presets WHERE status='pending' ORDER BY created_at ASC").all();
  const rows = results || [];
  const files = [{ name: "manifest.json", data: JSON.stringify({ exportedAt: nowIso(), submissions: rows }, null, 2) }];
  for (const row of rows) {
    const object = await env.PRESETS.get(row.r2_key);
    const ini = object ? await object.text() : "";
    const scan = scanPresetIni(ini);
    files.push({ name: `${row.id}/preset.ini`, data: ini });
    files.push({ name: `${row.id}/metadata.json`, data: JSON.stringify(row, null, 2) });
    files.push({ name: `${row.id}/scan.json`, data: JSON.stringify(scan, null, 2) });
  }
  return new Response(zipStore(files), {
    headers: {
      "content-type": "application/zip",
      "content-disposition": "attachment; filename=\"crimson-weather-pending-presets.zip\""
    }
  });
}

async function loginAdmin(request, env) {
  const body = await readJson(request);
  if (!env.ADMIN_TOKEN || !body || body.token !== env.ADMIN_TOKEN) {
    return bad("Invalid admin token.", 401);
  }
  const session = await makeAdminSession(env);
  return json({ ok: true }, 200, {
    "set-cookie": `cw_admin_session=${session}; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=86400`
  });
}

function logoutAdmin() {
  return json({ ok: true }, 200, {
    "set-cookie": "cw_admin_session=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0"
  });
}

const ADMIN_LOGIN_HTML = `<!doctype html>
<html><head><meta charset="utf-8"><title>Crimson Weather Admin Login</title>
<style>body{font-family:Segoe UI,Arial,sans-serif;margin:24px;background:#111;color:#eee}input,button{font:inherit;padding:8px;margin:4px 0}input{width:min(520px,100%);background:#0c0c0c;color:#eee;border:1px solid #333}button{display:block}.err{color:#ff9b9b}</style></head>
<body><h1>Crimson Weather Admin</h1>
<p>Paste your Worker ADMIN_TOKEN to create a local browser admin session.</p>
<input id="token" type="password" autocomplete="current-password" autofocus>
<button onclick="login()">Log In</button>
<p id="msg" class="err"></p>
<script>
async function login(){const token=document.getElementById('token').value; const r=await fetch('/api/v1/admin/login',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({token})}); if(r.ok){location.href='/admin';return;} document.getElementById('msg').textContent=await r.text();}
</script></body></html>`;

const ADMIN_HTML = `<!doctype html>
<html><head><meta charset="utf-8"><title>Crimson Weather Community Presets</title>
<style>
:root{color-scheme:dark}body{font-family:Segoe UI,Arial,sans-serif;margin:0;background:#111;color:#eee}main{max-width:1180px;margin:0 auto;padding:22px}button,input,select,textarea{font:inherit}button{margin:2px;padding:6px 10px;background:#2d4778;color:#fff;border:1px solid #5672aa;cursor:pointer}button.danger{background:#6b2424;border-color:#a64e4e}button.secondary{background:#252525;border-color:#555}.toolbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:12px 0}.row{border:1px solid #333;padding:12px;margin:10px 0;background:#181818}.row h3{margin:0 0 6px}.meta{color:#aaa;font-size:12px}.stats{float:right;color:#ddd}.pill{display:inline-block;padding:2px 7px;margin-left:6px;border:1px solid #555;background:#222;color:#ddd}.empty{color:#aaa;margin:16px 0}pre,textarea{width:100%;box-sizing:border-box;background:#0c0c0c;color:#eee;border:1px solid #333;padding:10px;white-space:pre-wrap}textarea{height:120px}.detail{position:fixed;inset:24px;background:#151515;border:1px solid #555;padding:16px;overflow:auto;box-shadow:0 16px 60px #000}.detail[hidden]{display:none}.detail header{display:flex;justify-content:space-between;gap:12px;align-items:center}.notice{color:#ffcf8a}
</style></head>
<body><main>
<h1>Crimson Weather Community Presets</h1>
<div class="toolbar">
  <select id="status" onchange="load()"><option value="pending">Pending</option><option value="approved">Approved</option><option value="rejected">Rejected</option><option value="all">All</option></select>
  <button onclick="load()" class="secondary">Refresh</button>
  <a href="/api/v1/admin/submissions/export.zip"><button class="secondary">Download Pending ZIP</button></a>
  <button onclick="rebuild()" class="secondary">Rebuild Catalog</button>
  <button onclick="logout()" class="secondary">Log Out</button>
</div>
<h2>Trusted Clients</h2>
<div class="toolbar">
  <input id="wlClientId" placeholder="Raw ClientId from addon" style="min-width:320px;background:#0c0c0c;color:#eee;border:1px solid #333;padding:7px">
  <input id="wlLabel" placeholder="Label" style="min-width:180px;background:#0c0c0c;color:#eee;border:1px solid #333;padding:7px">
  <button onclick="addWhitelist()">Whitelist ClientId</button>
  <button onclick="loadWhitelist()" class="secondary">Refresh Trusted</button>
</div>
<div id="whitelist" class="meta">Loading trusted clients...</div>
<h2>Presets</h2>
<div id="message" class="notice"></div>
<div id="list">Loading...</div>
<section id="detail" class="detail" hidden>
  <header><h2 id="detailTitle"></h2><button onclick="closeDetail()" class="secondary">Close</button></header>
  <div id="detailMeta" class="meta"></div>
  <h3>Metadata</h3>
  <pre id="detailJson"></pre>
  <h3>INI Content</h3>
  <pre id="detailIni"></pre>
  <h3>Deterministic Scan</h3>
  <pre id="detailScan"></pre>
</section>
</main>
<script>
let currentStatus='pending';
async function api(path, opts){const r=await fetch(path,opts); if(!r.ok) throw new Error(await r.text()); return r.json();}
function esc(s){return String(s??'').replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));}
function msg(text){document.getElementById('message').textContent=text||'';}
function rowHtml(row){
  const status='<span class="pill">'+esc(row.status)+'</span>';
  const stats='<span class="stats">'+Number(row.downloads||0)+' downloads | '+Number(row.likes||0)+' likes</span>';
  const clientHash=String(row.submitter_hash||'');
  const clientInfo=clientHash?'<div class=meta>Client hash '+esc(clientHash)+' <button onclick="whitelistPreset(\\''+row.id+'\\')" class="secondary">Whitelist Submitter</button></div>':'';
  const actions=[
    '<button onclick="viewPreset(\\''+row.id+'\\')" class="secondary">View Content</button>',
    row.status==='pending'?'<button onclick="approve(\\''+row.id+'\\')">Approve</button>':'',
    row.status==='pending'?'<button onclick="rejectOne(\\''+row.id+'\\')" class="secondary">Reject</button>':'',
    '<button onclick="deleteOne(\\''+row.id+'\\')" class="danger">Delete</button>'
  ].filter(Boolean).join(' ');
  const updateOf=row.update_of?'<div class=meta>Update for '+esc(row.update_of)+'</div>':'';
  return '<div class=row>'+stats+'<h3>'+esc(row.title)+status+'</h3><div class=meta>'+esc(row.id)+' by '+esc(row.author_name)+' | created '+esc(row.created_at)+' | updated '+esc(row.updated_at)+'</div>'+updateOf+clientInfo+'<p>'+esc(row.description||'')+'</p><p>'+actions+'</p></div>';
}
async function load(){
  currentStatus=document.getElementById('status').value;
  msg('');
  const data=await api('/api/v1/admin/submissions?status='+encodeURIComponent(currentStatus));
  document.getElementById('list').innerHTML=data.submissions.length?data.submissions.map(rowHtml).join(''):'<p class="empty">No presets here.</p>';
}
async function viewPreset(id){
  const data=await api('/api/v1/admin/presets/'+encodeURIComponent(id));
  document.getElementById('detailTitle').textContent=data.preset.title+' by '+data.preset.author_name;
  document.getElementById('detailMeta').textContent=data.preset.id+' | '+data.preset.status+' | '+data.preset.r2_key;
  document.getElementById('detailJson').textContent=JSON.stringify(data.preset,null,2);
  document.getElementById('detailIni').textContent=data.iniText||'(missing file)';
  document.getElementById('detailScan').textContent=JSON.stringify(data.scan,null,2);
  document.getElementById('detail').hidden=false;
}
function closeDetail(){document.getElementById('detail').hidden=true;}
async function approve(id){await api('/api/v1/admin/submissions/'+id+'/approve',{method:'POST'}); msg('Approved '+id); load();}
async function rejectOne(id){const note=prompt('Reject note?')||''; await api('/api/v1/admin/submissions/'+id+'/reject',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({note})}); msg('Rejected '+id); load();}
async function deleteOne(id){if(!confirm('Delete preset '+id+' from D1 and R2? This removes it from the public catalog.'))return; await api('/api/v1/admin/presets/'+encodeURIComponent(id),{method:'DELETE'}); msg('Deleted '+id); closeDetail(); load();}
async function loadWhitelist(){const data=await api('/api/v1/admin/whitelist'); document.getElementById('whitelist').innerHTML=data.clients.length?data.clients.map(c=>'<div>'+esc(c.label||'(unlabeled)')+' | '+esc(c.submitter_hash)+' | auto '+Number(c.auto_approve||0)+' <button onclick="deleteWhitelist(\\''+c.submitter_hash+'\\')" class="danger">Remove</button></div>').join(''):'No trusted clients yet.';}
async function addWhitelist(){const clientId=document.getElementById('wlClientId').value; const label=document.getElementById('wlLabel').value; await api('/api/v1/admin/whitelist',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({clientId,label,autoApprove:true})}); document.getElementById('wlClientId').value=''; msg('Client whitelisted'); loadWhitelist();}
async function whitelistPreset(id){const label=prompt('Trusted label?')||''; await api('/api/v1/admin/whitelist/from-preset/'+encodeURIComponent(id),{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({label})}); msg('Submitter whitelisted'); loadWhitelist();}
async function deleteWhitelist(hash){if(!confirm('Remove trusted client?'))return; await api('/api/v1/admin/whitelist/'+encodeURIComponent(hash),{method:'DELETE'}); msg('Trusted client removed'); loadWhitelist();}
async function rebuild(){await api('/api/v1/admin/catalog/rebuild',{method:'POST'}); msg('Catalog rebuilt');}
async function logout(){await api('/api/v1/admin/logout',{method:'POST'}); location.reload();}
load().catch(e=>document.getElementById('list').textContent=e.message);
loadWhitelist().catch(e=>document.getElementById('whitelist').textContent=e.message);
</script></body></html>`;

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const method = request.method.toUpperCase();
    try {
      if (method === "GET" && url.pathname === "/api/v1/catalog") return getCatalog(env);
      if (method === "POST" && url.pathname === "/api/v1/presets") return submitPreset(request, env);
      if (method === "GET" && url.pathname === "/api/v1/me/presets") return listMyPresets(request, env);
      let match = url.pathname.match(/^\/api\/v1\/me\/presets\/([^/]+)$/);
      if (method === "PUT" && match) return updateMyPreset(request, env, match[1]);
      if (method === "DELETE" && match) return deleteMyPreset(request, env, match[1]);
      match = url.pathname.match(/^\/api\/v1\/presets\/([^/]+)\/download$/);
      if (method === "GET" && match) return downloadPreset(request, env, match[1]);
      match = url.pathname.match(/^\/api\/v1\/presets\/([^/]+)\/like$/);
      if (method === "POST" && match) return toggleLike(request, env, match[1]);
      if (method === "POST" && url.pathname === "/api/v1/admin/login") return loginAdmin(request, env);
      if (method === "POST" && url.pathname === "/api/v1/admin/logout") return logoutAdmin();
      if (url.pathname === "/admin" && method === "GET") {
        if (!(await isAdmin(request, env))) return new Response(ADMIN_LOGIN_HTML, { headers: { "content-type": "text/html; charset=utf-8" } });
        return new Response(ADMIN_HTML, { headers: { "content-type": "text/html; charset=utf-8" } });
      }
      if (url.pathname.startsWith("/api/v1/admin/")) {
        if (!(await isAdmin(request, env))) return bad("Unauthorized", 401);
        if (method === "GET" && url.pathname === "/api/v1/admin/submissions") return listSubmissions(env, url.searchParams.get("status") || "pending");
        if (method === "GET" && url.pathname === "/api/v1/admin/submissions/export.zip") return exportPending(env);
        if (method === "POST" && url.pathname === "/api/v1/admin/catalog/rebuild") return json({ ok: true, catalog: await rebuildCatalog(env) });
        if (method === "POST" && url.pathname === "/api/v1/admin/submissions/batch") return batchSubmissions(request, env);
        if (method === "GET" && url.pathname === "/api/v1/admin/whitelist") return listWhitelist(env);
        if (method === "POST" && url.pathname === "/api/v1/admin/whitelist") return addWhitelist(request, env);
        match = url.pathname.match(/^\/api\/v1\/admin\/whitelist\/from-preset\/([^/]+)$/);
        if (method === "POST" && match) return whitelistFromPreset(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/whitelist\/([a-fA-F0-9]{64})$/);
        if (method === "DELETE" && match) return deleteWhitelist(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/presets\/([^/]+)$/);
        if (method === "GET" && match) return adminPresetDetail(env, match[1]);
        if (method === "DELETE" && match) return deletePresetAdmin(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/presets\/([^/]+)\/ini$/);
        if (method === "GET" && match) return adminPresetIni(env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/submissions\/([^/]+)\/approve$/);
        if (method === "POST" && match) return approveSubmission(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/submissions\/([^/]+)\/reject$/);
        if (method === "POST" && match) return rejectSubmission(request, env, match[1]);
      }
      return bad("Not found.", 404);
    } catch (error) {
      return bad(error?.message || "Server error.", 500);
    }
  }
};
