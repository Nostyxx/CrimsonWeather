import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { Miniflare } from "miniflare";
import { adminUploadUpdateArtifact, updateArtifact } from "../src/updates/artifacts.js";
import { adminSaveUpdateSettings, getUpdateSettings } from "../src/updates/settings.js";
import { sha256Hex } from "../src/shared/crypto.js";
import { createHash } from "node:crypto";
import { storeUpdateArtifact } from "../src/storage/update-artifacts.js";

// Direct handler tests use local D1/R2 bindings from Node. The separate Worker
// integration test below exercises the platform's native DigestStream.
class TestDigestStream extends WritableStream {
  constructor() {
    const hash = createHash("sha256");
    let resolve;
    let reject;
    const digest = new Promise((yes, no) => { resolve = yes; reject = no; });
    super({ write(chunk) { hash.update(chunk); }, close() { resolve(hash.digest()); }, abort(error) { reject(error); } });
    this.digest = digest;
  }
}

test("release publication preserves old artifacts on failures and publishes coherent settings", async (t) => {
  const previousDigestStream = crypto.DigestStream;
  crypto.DigestStream = TestDigestStream;
  t.after(() => { if (previousDigestStream) crypto.DigestStream = previousDigestStream; else delete crypto.DigestStream; });
  const runtime = new Miniflare({
    modules: true, script: "export default { fetch() { return new Response('test'); } };",
    compatibilityDate: "2026-05-20", d1Databases: ["DB"], r2Buckets: ["PRESETS"]
  });
  t.after(() => runtime.dispose());
  const env = { DB: await runtime.getD1Database("DB"), PRESETS: await runtime.getR2Bucket("PRESETS"), ADMIN_TOKEN: "test-token" };
  for (const migration of ["0001_initial.sql", "0002_whitelist_and_owner_updates.sql", "0003_admin_soft_delete.sql", "0004_update_settings.sql"]) {
    const sql = await readFile(new URL(`../migrations/${migration}`, import.meta.url), "utf8");
    for (const statement of sql.split(";").map((s) => s.trim()).filter(Boolean)) await env.DB.prepare(statement).run();
  }
  const upload = (bytes, target = env, version = "1.0.0") => adminUploadUpdateArtifact(new Request(
    `https://test.invalid/api/v1/admin/update/artifact?version=${version}`, {
      method: "PUT", headers: { authorization: "Bearer test-token" }, body: bytes
    }), target);
  const save = (version, target = env) => adminSaveUpdateSettings(new Request("https://test.invalid/api/v1/admin/update", {
    method: "PUT", body: JSON.stringify({ latestVersion: version, downloadPageUrl: "https://example.test/files", changelog: "Changes" })
  }), target);

  await t.test("chunked bodies exceeding the cap are cancelled and multipart state is aborted", async () => {
    let aborted = false;
    let cancelled = false;
    let remaining = 130;
    const chunk = new Uint8Array(1024 * 1024);
    const body = new ReadableStream({
      pull(controller) { if (remaining-- > 0) controller.enqueue(chunk); else controller.close(); },
      cancel() { cancelled = true; }
    });
    const bucket = { async createMultipartUpload() { return {
      async uploadPart(partNumber) { return { partNumber, etag: "part" }; },
      async complete() { assert.fail("oversized upload must not complete"); },
      async abort() { aborted = true; }
    }; } };
    await assert.rejects(storeUpdateArtifact(bucket, new Request("https://test.invalid", { method: "PUT", body, duplex: "half" }), "1.0.0"), /too large/);
    assert.equal(aborted, true);
    assert.equal(cancelled, true);
  });

  await t.test("a failed part upload aborts unpublished multipart data", async () => {
    let aborted = false;
    const bucket = { async createMultipartUpload() { return {
      async uploadPart() { throw new Error("part write failed"); },
      async complete() { assert.fail("failed upload must not complete"); },
      async abort() { aborted = true; }
    }; } };
    await assert.rejects(storeUpdateArtifact(bucket, new Request("https://test.invalid", { method: "PUT", body: "bytes" }), "1.0.0"), /part write failed/);
    assert.equal(aborted, true);
  });

  await t.test("a same-version re-upload retains the previous immutable artifact", async () => {
    const first = await (await upload("first artifact")).json();
    const second = await (await upload("second artifact")).json();
    assert.notEqual(first.addonR2Key, second.addonR2Key);
    assert.equal(await (await env.PRESETS.get(first.addonR2Key)).text(), "first artifact");
    const settings = await getUpdateSettings(env);
    assert.equal(settings.addonSha256, await sha256Hex("second artifact"));
    assert.equal(settings.addonR2Key, second.addonR2Key);
  });

  await t.test("R2 failure leaves settings and audit unchanged", async () => {
    const before = await getUpdateSettings(env);
    const failing = { ...env, PRESETS: { async createMultipartUpload() { throw new Error("injected R2 failure"); } } };
    await assert.rejects(upload("unwritten", failing, "2.0.0"), /injected R2 failure/);
    assert.deepEqual(await getUpdateSettings(env), before);
  });

  await t.test("D1 statement failure rolls back all settings and audit, and retry recovers", async () => {
    const before = await getUpdateSettings(env);
    const auditBefore = await env.DB.prepare("SELECT COUNT(*) AS count FROM admin_audit").first();
    await env.DB.prepare(`CREATE TRIGGER fail_artifact BEFORE UPDATE ON app_settings
      WHEN NEW.key='update.addonSha256' BEGIN SELECT RAISE(ABORT,'injected publication failure'); END`).run();
    await assert.rejects(upload("retryable", env, "2.0.0"), /injected publication failure/);
    assert.deepEqual(await getUpdateSettings(env), before);
    assert.deepEqual(await env.DB.prepare("SELECT COUNT(*) AS count FROM admin_audit").first(), auditBefore);
    assert.equal(await (await env.PRESETS.get(before.addonR2Key)).text(), "second artifact");
    await env.DB.prepare("DROP TRIGGER fail_artifact").run();
    assert.equal((await upload("retryable", env, "2.0.0")).status, 200);
    assert.equal((await getUpdateSettings(env)).latestVersion, "2.0.0");
  });

  await t.test("same-version settings save preserves artifact, new-version save clears environment fallbacks", async () => {
    const before = await getUpdateSettings(env);
    await save("2.0.0");
    assert.equal((await getUpdateSettings(env)).addonR2Key, before.addonR2Key);
    const fallback = { ...env, UPDATE_ADDON_R2_KEY: "legacy", UPDATE_ADDON_SHA256: "a".repeat(64), UPDATE_ADDON_SIZE_BYTES: "123" };
    await save("3.0.0", fallback);
    const cleared = await getUpdateSettings(fallback);
    assert.equal(cleared.addonR2Key, "");
    assert.equal(cleared.addonSha256, "");
    assert.equal(cleared.addonSizeBytes, 0);
  });

  await t.test("concurrent uploads never mix artifact key, size and hash", async () => {
    await Promise.all(Array.from({ length: 8 }, (_, i) => upload(`artifact-${i}`, env, `4.0.${i}`)));
    const settings = await getUpdateSettings(env);
    const object = await env.PRESETS.get(settings.addonR2Key);
    const bytes = await object.text();
    assert.equal(settings.addonSha256, await sha256Hex(bytes));
    assert.equal(settings.addonSizeBytes, new TextEncoder().encode(bytes).length);
    assert.ok(settings.addonR2Key.startsWith(`updates/${settings.latestVersion}/`));
    const response = await updateArtifact(new Request(`https://test.invalid/api/v1/update/artifact?version=${settings.latestVersion}`), env);
    assert.equal(response.status, 200);
    assert.equal(await response.text(), bytes);
  });
});
