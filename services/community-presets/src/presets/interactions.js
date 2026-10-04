import { bad, json } from "../http/responses.js";
import { hashClientId } from "../auth/client-identity.js";
import { nowIso } from "../shared/time.js";
import { rebuildCatalog } from "./catalog.js";

export async function downloadPreset(request, env, id) {
  const row = await env.DB.prepare("SELECT id,r2_key,status FROM presets WHERE id=? AND status='approved' AND deleted_at IS NULL").bind(id).first();
  if (!row) return bad("Preset not found.", 404);
  const object = await env.PRESETS.get(row.r2_key);
  if (!object) return bad("Preset file missing.", 404);
  const clientId = request.headers.get("x-cw-client-id") || "";
  if (clientId) {
    const deviceHash = await hashClientId(env, clientId);
    const day = new Date().toISOString().slice(0, 10);
    const countedAt = nowIso();
    const results = await env.DB.batch([
      env.DB.prepare("INSERT INTO preset_downloads_daily (preset_id,device_hash,day,created_at) VALUES (?,?,?,?) ON CONFLICT(preset_id,device_hash,day) DO NOTHING")
        .bind(id, deviceHash, day, countedAt),
      env.DB.prepare("UPDATE presets SET downloads=downloads+1, updated_at=? WHERE id=? AND status='approved' AND deleted_at IS NULL AND changes()>0")
        .bind(countedAt, id)
    ]);
    if (Number(results?.[1]?.meta?.changes || 0) > 0) {
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

export async function toggleLike(request, env, id) {
  const exists = await env.DB.prepare("SELECT id FROM presets WHERE id=? AND status='approved' AND deleted_at IS NULL").bind(id).first();
  if (!exists) return bad("Preset not found.", 404);
  const clientId = request.headers.get("x-cw-client-id") || "";
  if (!clientId) return bad("Missing anonymous client id.", 400);
  const deviceHash = await hashClientId(env, clientId);
  const changedAt = nowIso();
  const results = await env.DB.batch([
    env.DB.prepare(
      `DELETE FROM preset_likes WHERE preset_id=? AND device_hash=? AND EXISTS (
         SELECT 1 FROM presets WHERE id=? AND status='approved' AND deleted_at IS NULL
       )`
    ).bind(id, deviceHash, id),
    env.DB.prepare(
      `INSERT INTO preset_likes (preset_id,device_hash,created_at)
       SELECT ?,?,? WHERE changes()=0 AND EXISTS (
         SELECT 1 FROM presets WHERE id=? AND status='approved' AND deleted_at IS NULL
       )`
    ).bind(id, deviceHash, changedAt, id),
    env.DB.prepare(
      "UPDATE presets SET likes=(SELECT COUNT(*) FROM preset_likes WHERE preset_id=?), updated_at=? WHERE id=? AND status='approved' AND deleted_at IS NULL"
    ).bind(id, changedAt, id),
    env.DB.prepare(
      "SELECT p.likes,EXISTS (SELECT 1 FROM preset_likes l WHERE l.preset_id=p.id AND l.device_hash=?) AS liked FROM presets p WHERE p.id=? AND p.status='approved' AND p.deleted_at IS NULL"
    ).bind(deviceHash, id)
  ]);
  const row = results?.[3]?.results?.[0];
  if (!row) return bad("Preset not found.", 404);
  if (Number(results?.[2]?.meta?.changes || 0) === 0) return bad("Preset not found.", 404);
  await rebuildCatalog(env);
  return json({ ok: true, liked: Boolean(row.liked), likes: Number(row.likes || 0) });
}
