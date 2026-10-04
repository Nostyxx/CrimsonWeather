import { adminIdentity } from "../auth/admin-session.js";
import { nowIso } from "../shared/time.js";

export async function audit(env, request, action, id, note = "") {
  await env.DB.prepare("INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) VALUES (?,?,?,?,?)")
    .bind(action, id || "", adminIdentity(request, env), note, nowIso()).run();
}

export async function systemAudit(env, action, id, note = "") {
  await env.DB.prepare("INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) VALUES (?,?,?,?,?)")
    .bind(action, id || "", "system", note, nowIso()).run();
}
