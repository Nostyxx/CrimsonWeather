import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import worker from "../src/index.js";
import { routeBaseline } from "./fixtures/route-baseline.mjs";
import { hmacHex } from "../src/shared/crypto.js";

function environment() {
  return {
    DB: { prepare() { return {
      bind() { return this; },
      async first() { return null; },
      async all() { return { results: [] }; },
      async run() { return { success: true }; }
    }; } },
    PRESETS: { async get() { return null; } },
    ADMIN_TOKEN: "review-token",
    ADMIN_LOGIN_KEY: "review-key",
    DEVICE_HASH_SECRET: "review-only-secret"
  };
}

for (const fixture of routeBaseline) {
  test(`original API response: ${fixture.method} ${fixture.path}${fixture.headers ? " (authenticated)" : ""}`, async () => {
    const response = await worker.fetch(new Request(`https://review.test${fixture.path}`, {
      method: fixture.method, body: fixture.body, headers: fixture.headers
    }), environment());
    const body = await response.text();
    assert.equal(response.status, fixture.expected.status);
    assert.deepEqual([...response.headers], fixture.expected.headers);
    if (fixture.expected.bodySha256) {
      assert.equal(createHash("sha256").update(body).digest("hex"), fixture.expected.bodySha256);
    } else assert.equal(body, fixture.expected.body);
  });
}

test("login creates a usable secure session and expired/tampered sessions fail", async () => {
  const env = environment();
  const login = await worker.fetch(new Request("https://review.test/api/v1/admin/login", {
    method: "POST", headers: { "content-type": "application/json" },
    body: JSON.stringify({ token: env.ADMIN_TOKEN, loginKey: env.ADMIN_LOGIN_KEY })
  }), env);
  assert.equal(login.status, 200);
  const cookie = login.headers.get("set-cookie");
  for (const attribute of ["HttpOnly", "Secure", "SameSite=Strict", "Path=/", "Max-Age=86400"]) {
    assert.ok(cookie.includes(attribute), attribute);
  }
  const request = (session) => new Request("https://review.test/api/v1/admin/audit", { headers: { cookie: session } });
  assert.equal((await worker.fetch(request(cookie.split(";")[0]), env)).status, 200);
  assert.equal((await worker.fetch(request("cw_admin_session=invalid.invalid"), env)).status, 401);
  const expiredPayload = Buffer.from(JSON.stringify({ exp: Date.now() - 60000 })).toString("base64url");
  const signature = await hmacHex(env.ADMIN_TOKEN, expiredPayload);
  assert.equal((await worker.fetch(request(`cw_admin_session=${expiredPayload}.${signature}`), env)).status, 401);
});

test("admin auth precedes dispatch for every protected method", async () => {
  for (const method of ["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"]) {
    const response = await worker.fetch(new Request("https://review.test/api/v1/admin/not-real", { method }), environment());
    assert.equal(response.status, 401, method);
  }
});

test("unconfigured login key shows login page and existing Access header grants admin access", async () => {
  const env = environment();
  delete env.ADMIN_LOGIN_KEY;
  assert.equal((await worker.fetch(new Request("https://review.test/admin"), env)).status, 200);
  const response = await worker.fetch(new Request("https://review.test/api/v1/admin/audit", {
    headers: { "cf-access-authenticated-user-email": "fixture@example.test" }
  }), env);
  assert.equal(response.status, 200);
});
