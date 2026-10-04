import { readJson } from "../http/request-validation.js";
import { approveSubmission, purgePresetAdmin, rejectSubmission, restorePresetAdmin, softDeletePreset } from "../presets/moderation.js";
import { json } from "../http/responses.js";
import { whitelistSubmitterFromPreset } from "./whitelist.js";

export async function batchSubmissions(request, env) {
  const body = await readJson(request);
  const approvals = Array.isArray(body?.approve) ? body.approve : [];
  const rejections = Array.isArray(body?.reject) ? body.reject : [];
  const deletions = Array.isArray(body?.delete) ? body.delete : [];
  const restores = Array.isArray(body?.restore) ? body.restore : [];
  const purges = Array.isArray(body?.purge) ? body.purge : [];
  const whitelists = Array.isArray(body?.whitelist) ? body.whitelist : [];
  const results = [];
  for (const id of approvals) results.push(await (await approveSubmission(request, env, String(id))).json());
  for (const id of rejections) results.push(await (await rejectSubmission(request, env, String(id))).json());
  for (const id of deletions) results.push(await (await softDeletePreset(request, env, String(id), "batch")).json());
  for (const id of restores) results.push(await (await restorePresetAdmin(request, env, String(id))).json());
  for (const id of purges) results.push(await (await purgePresetAdmin(request, env, String(id), "batch-purge")).json());
  for (const id of whitelists) results.push(await (await whitelistSubmitterFromPreset(env, request, String(id))).json());
  return json({ ok: true, results });
}
