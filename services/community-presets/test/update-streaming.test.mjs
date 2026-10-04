import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import { createHash } from "node:crypto";
import { build } from "esbuild";
import { Miniflare } from "miniflare";

test("native Workers streaming upload hashes multiple parts and enforces empty/size checks", async (t) => {
  const bundle = await build({
    entryPoints: [fileURLToPath(new URL("../src/index.js", import.meta.url))],
    bundle: true, write: false, platform: "browser", format: "esm", target: "es2022"
  });
  const runtime = new Miniflare({ modules: true, script: bundle.outputFiles[0].text,
    compatibilityDate: "2026-05-20", d1Databases: ["DB"], r2Buckets: ["PRESETS"], bindings: { ADMIN_TOKEN: "test-token" } });
  t.after(() => runtime.dispose());
  const db = await runtime.getD1Database("DB");
  for (const migration of ["0001_initial.sql", "0002_whitelist_and_owner_updates.sql", "0003_admin_soft_delete.sql", "0004_update_settings.sql"]) {
    const sql = await readFile(new URL(`../migrations/${migration}`, import.meta.url), "utf8");
    for (const statement of sql.split(";").map((s) => s.trim()).filter(Boolean)) await db.prepare(statement).run();
  }
  const upload = (body, headers = {}) => runtime.dispatchFetch("https://test.invalid/api/v1/admin/update/artifact?version=1.2.3", {
    method: "PUT", headers: { authorization: "Bearer test-token", ...headers }, body
  });
  const bytes = new Uint8Array(6 * 1024 * 1024 + 13).fill(0x65);
  bytes[bytes.length - 1] = 0x66;
  const response = await upload(bytes);
  const result = await response.json();
  assert.equal(response.status, 200, JSON.stringify(result));
  assert.equal(result.addonSizeBytes, bytes.length);
  assert.equal(result.addonSha256, createHash("sha256").update(bytes).digest("hex"));
  const downloaded = await runtime.dispatchFetch("https://test.invalid/api/v1/update/artifact?version=1.2.3");
  assert.equal(downloaded.status, 200);
  assert.equal(createHash("sha256").update(new Uint8Array(await downloaded.arrayBuffer())).digest("hex"), result.addonSha256);
  const empty = await upload("");
  assert.equal(empty.status, 400);
  assert.equal((await empty.json()).error, "Addon file is empty.");
  const mismatched = await runtime.dispatchFetch("https://test.invalid/api/v1/update/artifact?version=9.9.9");
  assert.equal(mismatched.status, 404);
});
