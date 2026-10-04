// Security tests for the admin Worker's authentication and the public Worker's
// lack of any admin surface.
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { startWorker, TEST_ENV } from "./harness.mjs";
import { IMPLEMENTATIONS, prepare } from "./run.mjs";
import { ACCESS_BINDINGS, ADMIN_EMAIL, AUDIENCE, TEAM_DOMAIN, accessJwt, accessOutbound, foreignKey } from "./access-fixture.mjs";

const impl = IMPLEMENTATIONS.next;
let worker;
let unconfigured;

before(async () => {
  await prepare(impl);
  worker = await startWorker(impl);
  // Same Workers, but the admin Worker has no Access configuration at all.
  unconfigured = await startWorker({ ...impl, adminBindings: {} });
});
after(async () => {
  await worker?.dispose();
  await unconfigured?.dispose();
});

const status = async (w, path, token, init = {}) => {
  const headers = { ...(init.headers || {}) };
  if (token !== undefined) headers["cf-access-jwt-assertion"] = token;
  return (await w.adminFetch(path, { ...init, headers })).status;
};

test("valid Access token for an allowed email is accepted", async () => {
  assert.equal(await status(worker, "/api/admin/overview", await accessJwt()), 200);
  assert.equal(await status(worker, "/", await accessJwt()), 200);
});

test("requests without a valid Access token are refused", async () => {
  const cases = {
    "no token": undefined,
    "empty token": "",
    "garbage": "not.a.jwt",
    "signed by another key": await accessJwt({}, { key: foreignKey }),
    "unknown key id": await accessJwt({}, { kid: "other-key" }),
    "wrong audience": await accessJwt({ aud: ["someone-else"] }),
    "wrong issuer": await accessJwt({ iss: "https://evil.cloudflareaccess.com" }),
    "expired": await accessJwt({ exp: Math.floor(Date.now() / 1000) - 10 }),
    "not yet valid": await accessJwt({ nbf: Math.floor(Date.now() / 1000) + 3600 }),
    "email not allowed": await accessJwt({ email: "intruder@example.com" }),
    "no identity": await accessJwt({ email: undefined }),
    "unknown service token": await accessJwt({ email: undefined, common_name: "someone.access" }),
  };
  for (const [name, token] of Object.entries(cases)) {
    assert.equal(await status(worker, "/api/admin/overview", token), 403, name);
  }
});

test("unsigned and HMAC tokens are refused", async () => {
  const b64 = (o) => Buffer.from(JSON.stringify(o)).toString("base64url");
  const claims = { aud: [AUDIENCE], iss: `https://${TEAM_DOMAIN}`, email: ADMIN_EMAIL, exp: Math.floor(Date.now() / 1000) + 60 };
  assert.equal(await status(worker, "/api/admin/overview", `${b64({ alg: "none", kid: "test-key-1" })}.${b64(claims)}.`), 403);
  assert.equal(await status(worker, "/api/admin/overview", `${b64({ alg: "HS256", kid: "test-key-1" })}.${b64(claims)}.c2ln`), 403);
});

test("allowed service token (scripts) is accepted", async () => {
  const token = await accessJwt({ email: undefined, common_name: "release-script.access" });
  assert.equal(await status(worker, "/api/admin/overview", token), 200);
});

test("admin Worker without Access configuration refuses everyone", async () => {
  assert.equal(await status(unconfigured, "/api/admin/overview", await accessJwt()), 403);
  assert.equal(await status(unconfigured, "/", await accessJwt()), 403);
});

test("legacy tricks do not work on the admin Worker", async () => {
  const headers = { "cf-access-authenticated-user-email": ADMIN_EMAIL, authorization: `Bearer ${TEST_ENV.ADMIN_TOKEN}` };
  assert.equal(await status(worker, "/api/admin/overview", undefined, { headers }), 403);
});

// Miniflare itself rejects requests carrying an Origin header, so the check is
// exercised directly rather than through the runtime.
test("cross-origin state changes are refused", async () => {
  const { isAllowedOrigin } = await import("../src/admin/origin.js");
  const req = (method, origin) =>
    new Request("https://admin.example.dev/api/admin/catalog/rebuild", { method, headers: origin ? { origin } : {} });
  assert.equal(isAllowedOrigin(req("POST", "https://evil.example")), false);
  assert.equal(isAllowedOrigin(req("DELETE", "https://admin.example.dev.evil.example")), false);
  assert.equal(isAllowedOrigin(req("POST", "null")), false);
  assert.equal(isAllowedOrigin(req("POST", "https://admin.example.dev")), true);
  assert.equal(isAllowedOrigin(req("POST")), true); // scripts send no Origin
  assert.equal(isAllowedOrigin(req("GET", "https://evil.example")), true); // reads change nothing
  // And a POST without Origin works end to end.
  assert.equal(await status(worker, "/api/admin/catalog/rebuild", await accessJwt(), { method: "POST" }), 200);
});

test("admin responses carry security headers", async () => {
  const res = await worker.adminFetch("/", { headers: { "cf-access-jwt-assertion": await accessJwt() } });
  assert.match(res.headers.get("content-security-policy") || "", /frame-ancestors 'none'/);
  assert.equal(res.headers.get("cache-control"), "no-store");
  assert.equal(res.headers.get("x-content-type-options"), "nosniff");
});

test("public Worker exposes no admin functionality", async () => {
  const attempts = [
    ["GET", "/admin", {}],
    ["GET", "/admin?key=" + TEST_ENV.ADMIN_LOGIN_KEY, {}],
    ["GET", "/api/v1/admin/overview", { authorization: `Bearer ${TEST_ENV.ADMIN_TOKEN}` }],
    ["GET", "/api/v1/admin/overview", { "cf-access-authenticated-user-email": ADMIN_EMAIL }],
    ["POST", "/api/v1/admin/login", { "content-type": "application/json" }],
    ["PUT", "/api/v1/admin/update/artifact?version=9.9.9", { authorization: `Bearer ${TEST_ENV.ADMIN_TOKEN}` }],
    ["GET", "/api/admin/overview", { "cf-access-jwt-assertion": await accessJwt() }],
  ];
  for (const [method, path, headers] of attempts) {
    const res = await worker.fetch(path, { method, headers, body: method === "GET" ? undefined : "{}" });
    assert.equal(res.status, 404, `${method} ${path}`);
  }
});

test("Access bindings used by the fixture match what the Worker reads", () => {
  assert.deepEqual(Object.keys(ACCESS_BINDINGS).sort(), ["ACCESS_AUD", "ACCESS_TEAM_DOMAIN", "ADMIN_EMAILS", "ADMIN_SERVICE_TOKENS"]);
  assert.equal(typeof accessOutbound, "function");
});
