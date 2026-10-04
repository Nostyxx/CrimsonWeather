// Admin adapters: scenarios need moderation steps (approve, whitelist, publish an
// update), but each implementation exposes its own admin API. An adapter maps the
// scenario's intent onto one implementation. Admin responses are not recorded;
// only their effects on the public API and on stored data are.
import { TEST_ENV, sleep } from "./harness.mjs";
import { accessJwt } from "./access-fixture.mjs";

async function expectOk(res, what) {
  if (!res.ok) throw new Error(`${what} failed: ${res.status} ${await res.text()}`);
  return res;
}

// The rewrite's admin Worker, authenticated with a (test) Cloudflare Access token.
export function nextAdmin(worker) {
  const call = async (what, method, path, init = {}) => {
    await sleep(4);
    const headers = { "cf-access-jwt-assertion": await accessJwt(), ...(init.headers || {}) };
    return worker.adminFetch(path, { method, ...init, headers }).then((r) => expectOk(r, what));
  };
  const jsonBody = (body) => ({ headers: { "content-type": "application/json" }, body: JSON.stringify(body) });
  const id = encodeURIComponent;
  const action = (name, body = {}) => (presetId) =>
    call(name, "POST", `/api/admin/presets/${id(presetId)}/${name}`, jsonBody(body));

  return {
    approve: action("approve"),
    reject: (presetId, note = "") => action("reject", { note })(presetId),
    trustSubmitterOf: action("trust"),
    softDelete: action("delete", { reason: "admin" }),
    restore: action("restore"),
    purge: action("purge"),
    publishUpdate: (settings) => call("publish update", "PUT", "/api/admin/release", jsonBody(settings)),
    uploadArtifact: (version, bytes) => call("upload artifact", "PUT", `/api/admin/release/artifact?version=${id(version)}`, {
      headers: { "content-type": "application/octet-stream" },
      body: bytes,
    }),
  };
}

export function legacyAdmin(worker) {
  const auth = { authorization: `Bearer ${TEST_ENV.ADMIN_TOKEN}` };
  const call = async (what, method, path, init = {}) => {
    await sleep(4);
    return worker.fetch(path, { method, ...init, headers: { ...auth, ...(init.headers || {}) } }).then((r) => expectOk(r, what));
  };
  const jsonBody = (body) => ({ headers: { "content-type": "application/json" }, body: JSON.stringify(body) });
  const id = encodeURIComponent;

  return {
    approve: (presetId) => call("approve", "POST", `/api/v1/admin/submissions/${id(presetId)}/approve`),
    reject: (presetId, note = "") =>
      call("reject", "POST", `/api/v1/admin/submissions/${id(presetId)}/reject`, jsonBody({ note })),
    trustSubmitterOf: (presetId) =>
      call("whitelist", "POST", `/api/v1/admin/whitelist/from-preset/${id(presetId)}`, jsonBody({ label: "" })),
    softDelete: (presetId) => call("soft delete", "DELETE", `/api/v1/admin/presets/${id(presetId)}`, jsonBody({ reason: "admin" })),
    restore: (presetId) => call("restore", "POST", `/api/v1/admin/presets/${id(presetId)}/restore`),
    purge: (presetId) => call("purge", "POST", `/api/v1/admin/presets/${id(presetId)}/purge`),
    publishUpdate: (settings) => call("publish update", "PUT", "/api/v1/admin/update", jsonBody(settings)),
    uploadArtifact: (version, bytes) => call("upload artifact", "PUT", `/api/v1/admin/update/artifact?version=${id(version)}`, {
      headers: { "content-type": "application/octet-stream" },
      body: bytes,
    }),
  };
}
