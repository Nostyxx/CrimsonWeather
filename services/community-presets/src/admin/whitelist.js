import { bad, json } from "../http/responses.js";
import { nowIso } from "../shared/time.js";
import { sanitizeText } from "../shared/values.js";
import { audit } from "../storage/audit-repository.js";
import { clientFingerprint, hashClientId, withClientFingerprint } from "../auth/client-identity.js";
import { readJson } from "../http/request-validation.js";

export async function whitelistSubmitterFromPreset(env, request, id, label = "", note = "") {
  const row = await env.DB.prepare("SELECT id,title,author_name,submitter_hash FROM presets WHERE id=?").bind(id).first();
  if (!row || !row.submitter_hash) return bad("Preset submitter was not found.", 404);
  const now = nowIso();
  const finalLabel = sanitizeText(label, 80) || row.author_name || row.title || id;
  const finalNote = sanitizeText(note, 300);
  await env.DB.prepare(
    "INSERT INTO client_whitelist (submitter_hash,label,auto_approve,note,created_at,updated_at) VALUES (?,?,?,?,?,?) ON CONFLICT(submitter_hash) DO UPDATE SET label=excluded.label,auto_approve=excluded.auto_approve,note=excluded.note,updated_at=excluded.updated_at"
  ).bind(row.submitter_hash, finalLabel, 1, finalNote, now, now).run();
  await audit(env, request, "whitelist-from-preset", id, clientFingerprint(row.submitter_hash));
  return json({ ok: true, submitterHash: row.submitter_hash, fingerprint: clientFingerprint(row.submitter_hash), label: finalLabel });
}

export async function listWhitelist(env) {
  const { results } = await env.DB.prepare("SELECT * FROM client_whitelist ORDER BY updated_at DESC LIMIT 200").all();
  return json({ ok: true, clients: (results || []).map(withClientFingerprint) });
}

export async function addWhitelist(request, env) {
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

export async function whitelistFromPreset(request, env, id) {
  const body = await readJson(request) || {};
  return whitelistSubmitterFromPreset(env, request, id, body.label || "", body.note || "");
}

export async function deleteWhitelist(request, env, submitterHash) {
  if (!/^[a-f0-9]{64}$/i.test(submitterHash)) return bad("Invalid submitter hash.", 400);
  await env.DB.prepare("DELETE FROM client_whitelist WHERE submitter_hash=?").bind(submitterHash.toLowerCase()).run();
  await audit(env, request, "whitelist-delete", "", clientFingerprint(submitterHash));
  return json({ ok: true, deleted: true });
}
