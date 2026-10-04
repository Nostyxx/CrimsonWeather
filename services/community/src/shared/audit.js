// Append-only log of moderation and system actions (shown in the admin tool).
import { nowIso } from "./values.js";

// actor: "admin:<email>" for people, "system" / "scheduled-worker" for automation.
export function auditStatement(env, actor, action, presetId = "", note = "") {
  return env.DB.prepare(
    "INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at) VALUES (?,?,?,?,?)",
  ).bind(action, presetId || "", actor, note, nowIso());
}

export const audit = (env, actor, action, presetId, note) => auditStatement(env, actor, action, presetId, note).run();
