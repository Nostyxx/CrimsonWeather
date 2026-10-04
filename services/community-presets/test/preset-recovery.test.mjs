import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { Miniflare } from "miniflare";
import { submitPreset, updateMyPreset } from "../src/presets/submissions.js";
import { approveSubmission, purgePresetAdmin } from "../src/presets/moderation.js";
import { hashClientId } from "../src/auth/client-identity.js";
import { sha256Hex } from "../src/shared/crypto.js";
import { drainAbandonedPresetStaging, drainPresetObjectCleanup, stagePresetObject } from "../src/storage/preset-objects.js";
import { purgeExpiredDeletedPresets } from "../src/jobs/purge-deleted.js";

const ini = "[CrimsonWeatherPreset]\n[Meta]\nFormatVersion=6\n[Weather]\nRain=0.2500\n";
const owner = "recovery-install";
const ownerRequest = (method, path, title, text = ini) => new Request(`https://test.invalid${path}`, {
  method, headers: { "x-cw-client-id": owner, "content-type": "application/json" },
  body: JSON.stringify({ title, authorName: "Author", iniText: text })
});
const adminRequest = (path) => new Request(`https://test.invalid${path}`, {
  method: "POST", headers: { authorization: "Bearer test-token" }
});

test("preset publication keeps referenced objects intact and recovers staged and retired objects", async (t) => {
  const runtime = new Miniflare({ modules: true, script: "export default { fetch() { return new Response('test'); } };",
    compatibilityDate: "2026-05-20", d1Databases: ["DB"], r2Buckets: ["PRESETS"] });
  t.after(() => runtime.dispose());
  const env = { DB: await runtime.getD1Database("DB"), PRESETS: await runtime.getR2Bucket("PRESETS"),
    ADMIN_TOKEN: "test-token", DEVICE_HASH_SECRET: "test-secret" };
  for (const migration of ["0001_initial.sql", "0002_whitelist_and_owner_updates.sql", "0003_admin_soft_delete.sql",
    "0004_update_settings.sql", "0005_preset_object_recovery.sql"]) {
    const sql = await readFile(new URL(`../migrations/${migration}`, import.meta.url), "utf8");
    for (const statement of sql.split(";").map((s) => s.trim()).filter(Boolean)) await env.DB.prepare(statement).run();
  }

  await t.test("D1 insert failure leaves discoverable staging, and cleanup removes it", async () => {
    await env.DB.prepare(`CREATE TRIGGER fail_submit BEFORE INSERT ON presets WHEN NEW.title='Failure'
      BEGIN SELECT RAISE(ABORT,'injected submit failure'); END`).run();
    await assert.rejects(submitPreset(ownerRequest("POST", "/api/v1/presets", "Failure"), env), /injected submit failure/);
    const staged = await env.DB.prepare("SELECT r2_key FROM preset_object_staging").first();
    assert.ok(staged?.r2_key);
    assert.ok(await env.PRESETS.get(staged.r2_key));
    assert.equal(await env.DB.prepare("SELECT COUNT(*) AS n FROM presets").first().then((r) => r.n), 0);
    await env.DB.prepare("DROP TRIGGER fail_submit").run();
    await drainAbandonedPresetStaging(env, "9999-01-01T00:00:00.000Z");
    assert.equal(await env.PRESETS.get(staged.r2_key), null);
    assert.equal(await env.DB.prepare("SELECT COUNT(*) AS n FROM preset_object_staging").first().then((r) => r.n), 0);
  });

  await t.test("staging cleanup retains a retry record when R2 deletion fails", async () => {
    const key = `pending/recovery/revisions/${crypto.randomUUID()}/preset.ini`;
    await stagePresetObject(env, key, new TextEncoder().encode(ini));
    const failingBucket = { delete: async () => { throw new Error("injected staged delete failure"); } };
    await assert.rejects(drainAbandonedPresetStaging({ ...env, PRESETS: failingBucket }, "9999-01-01T00:00:00.000Z"),
      /injected staged delete failure/);
    assert.equal(await env.DB.prepare("SELECT r2_key FROM preset_object_staging WHERE r2_key=?").bind(key).first(), null);
    assert.ok(await env.DB.prepare("SELECT r2_key FROM preset_object_cleanup WHERE r2_key=?").bind(key).first());
    await drainPresetObjectCleanup(env);
    assert.equal(await env.PRESETS.get(key), null);
  });

  await t.test("revoked whitelist access cannot publish a trusted submission or lose staging", async () => {
    const hash = await hashClientId(env, owner);
    const at = new Date().toISOString();
    await env.DB.prepare("INSERT INTO client_whitelist (submitter_hash,auto_approve,created_at,updated_at) VALUES (?,1,?,?)")
      .bind(hash, at, at).run();
    const bucket = {
      put: async (...args) => {
        const result = await env.PRESETS.put(...args);
        await env.DB.prepare("DELETE FROM client_whitelist WHERE submitter_hash=?").bind(hash).run();
        return result;
      }
    };
    const result = await submitPreset(ownerRequest("POST", "/api/v1/presets", "Revoked"), { ...env, PRESETS: bucket });
    assert.equal(result.status, 409);
    assert.equal(await env.DB.prepare("SELECT id FROM presets WHERE title='Revoked'").first(), null);
    const staged = await env.DB.prepare("SELECT r2_key FROM preset_object_staging").first();
    assert.ok(staged?.r2_key);
    await drainAbandonedPresetStaging(env, "9999-01-01T00:00:00.000Z");
    assert.equal(await env.PRESETS.get(staged.r2_key), null);
  });

  const submitted = await (await submitPreset(ownerRequest("POST", "/api/v1/presets", "Original"), env)).json();
  const id = submitted.id;
  const pending = await env.DB.prepare("SELECT * FROM presets WHERE id=?").bind(id).first();
  assert.ok(await env.PRESETS.get(pending.r2_key));
  assert.equal((await approveSubmission(adminRequest(`/api/v1/admin/submissions/${id}/approve`), env, id)).status, 200);
  const approved = await env.DB.prepare("SELECT * FROM presets WHERE id=?").bind(id).first();
  assert.equal(await (await env.PRESETS.get(approved.r2_key)).text(), ini);
  assert.notEqual(approved.r2_key, pending.r2_key);

  const pendingUpdate = await (await updateMyPreset(ownerRequest("PUT", `/api/v1/me/presets/${id}`, "Edited", ini.replace("0.2500", "0.7500")), env, id)).json();
  const updateRow = await env.DB.prepare("SELECT * FROM presets WHERE id=?").bind(pendingUpdate.updateId).first();
  const oldBytes = await (await env.PRESETS.get(approved.r2_key)).text();

  await t.test("a failed second R2 write leaves target and pending rows unchanged", async () => {
    let puts = 0;
    const failingBucket = {
      get: (...args) => env.PRESETS.get(...args),
      put: (...args) => ++puts === 2 ? Promise.reject(new Error("injected R2 history failure")) : env.PRESETS.put(...args)
    };
    await assert.rejects(approveSubmission(adminRequest(`/api/v1/admin/submissions/${updateRow.id}/approve`),
      { ...env, PRESETS: failingBucket }, updateRow.id), /injected R2 history failure/);
    assert.equal((await env.DB.prepare("SELECT r2_key FROM presets WHERE id=?").bind(id).first()).r2_key, approved.r2_key);
    assert.equal((await env.DB.prepare("SELECT status FROM presets WHERE id=?").bind(updateRow.id).first()).status, "pending");
    assert.equal(await (await env.PRESETS.get(approved.r2_key)).text(), oldBytes);
    assert.ok((await env.DB.prepare("SELECT COUNT(*) AS n FROM preset_object_staging").first()).n >= 2);
    await drainAbandonedPresetStaging(env, "9999-01-01T00:00:00.000Z");
  });

  await t.test("a failed D1 approval rolls back both rows and can be retried", async () => {
    await env.DB.prepare(`CREATE TRIGGER fail_approve BEFORE UPDATE ON presets WHEN NEW.id='${id}' AND NEW.status='approved'
      BEGIN SELECT RAISE(ABORT,'injected approval failure'); END`).run();
    await assert.rejects(approveSubmission(adminRequest(`/api/v1/admin/submissions/${updateRow.id}/approve`), env, updateRow.id),
      /injected approval failure/);
    assert.equal((await env.DB.prepare("SELECT r2_key FROM presets WHERE id=?").bind(id).first()).r2_key, approved.r2_key);
    assert.equal((await env.DB.prepare("SELECT status FROM presets WHERE id=?").bind(updateRow.id).first()).status, "pending");
    await env.DB.prepare("DROP TRIGGER fail_approve").run();
    await drainAbandonedPresetStaging(env, "9999-01-01T00:00:00.000Z");
    assert.equal((await approveSubmission(adminRequest(`/api/v1/admin/submissions/${updateRow.id}/approve`), env, updateRow.id)).status, 200);
    const active = await env.DB.prepare("SELECT r2_key FROM presets WHERE id=?").bind(id).first();
    assert.notEqual(active.r2_key, approved.r2_key);
    assert.equal(await (await env.PRESETS.get(active.r2_key)).text(), ini.replace("0.2500", "0.7500"));
    assert.equal(await (await env.PRESETS.get(approved.r2_key)).text(), oldBytes);
    await drainPresetObjectCleanup(env);
    assert.equal(await env.PRESETS.get(approved.r2_key), null);
  });

  await t.test("concurrent trusted edits reject a stale revision without changing active bytes", async () => {
    const hash = await hashClientId(env, owner);
    await env.DB.prepare("INSERT INTO client_whitelist (submitter_hash,auto_approve,created_at,updated_at) VALUES (?,1,?,?)")
      .bind(hash, new Date().toISOString(), new Date().toISOString()).run();
    let waiting = 0;
    let release;
    const barrier = new Promise((resolve) => { release = resolve; });
    const bucket = {
      put: async (...args) => {
        if (args[0].startsWith(`approved/${id}/`)) {
          waiting += 1;
          if (waiting === 2) release();
          await barrier;
        }
        return env.PRESETS.put(...args);
      },
      get: (...args) => env.PRESETS.get(...args),
      head: (...args) => env.PRESETS.head(...args)
    };
    const request = (title) => ownerRequest("PUT", `/api/v1/me/presets/${id}`, title, ini.replace("0.2500", title === "First" ? "0.1000" : "0.9000"));
    const outcomes = await Promise.all([updateMyPreset(request("First"), { ...env, PRESETS: bucket }, id),
      updateMyPreset(request("Second"), { ...env, PRESETS: bucket }, id)]);
    assert.deepEqual(outcomes.map((r) => r.status).sort(), [200, 409]);
    const active = await env.DB.prepare("SELECT r2_key,sha256 FROM presets WHERE id=?").bind(id).first();
    const object = await env.PRESETS.get(active.r2_key);
    assert.ok(object);
    assert.ok(["First", "Second"].includes((await env.DB.prepare("SELECT title FROM presets WHERE id=?").bind(id).first()).title));
    await drainAbandonedPresetStaging(env, "9999-01-01T00:00:00.000Z");
    assert.equal(await (await env.PRESETS.get(active.r2_key)).text(), await object.text());
  });

  await t.test("purge commits D1 and retries R2 cleanup after a delete failure", async () => {
    const active = await env.DB.prepare("SELECT r2_key FROM presets WHERE id=?").bind(id).first();
    const failingBucket = {
      delete: async () => { throw new Error("injected delete failure"); },
      head: (...args) => env.PRESETS.head(...args),
      put: (...args) => env.PRESETS.put(...args),
      get: (...args) => env.PRESETS.get(...args)
    };
    const originalError = console.error;
    let deferredLogged = false;
    console.error = (message) => { deferredLogged = String(message).includes("preset_object_cleanup_deferred"); };
    let result;
    try {
      result = await purgePresetAdmin(adminRequest(`/api/v1/admin/presets/${id}/purge`),
        { ...env, PRESETS: failingBucket }, id);
    } finally {
      console.error = originalError;
    }
    assert.equal(result.status, 200);
    assert.equal(deferredLogged, true);
    assert.equal(await env.DB.prepare("SELECT id FROM presets WHERE id=?").bind(id).first(), null);
    assert.ok((await env.DB.prepare("SELECT COUNT(*) AS n FROM preset_object_cleanup").first()).n > 0);
    await drainPresetObjectCleanup(env);
    assert.equal(await env.PRESETS.get(active.r2_key), null);
    assert.equal((await env.DB.prepare("SELECT COUNT(*) AS n FROM preset_object_cleanup").first()).n, 0);
  });

  await t.test("scheduled retention purges expired rows and their current object", async () => {
    const submitted = await (await submitPreset(ownerRequest("POST", "/api/v1/presets", "Expired"), env)).json();
    const row = await env.DB.prepare("SELECT r2_key FROM presets WHERE id=?").bind(submitted.id).first();
    await env.DB.prepare("UPDATE presets SET deleted_at=?,delete_after=? WHERE id=?")
      .bind("2000-01-01T00:00:00.000Z", "2000-01-02T00:00:00.000Z", submitted.id).run();
    assert.equal((await purgeExpiredDeletedPresets(env)).purged, 1);
    assert.equal(await env.DB.prepare("SELECT id FROM presets WHERE id=?").bind(submitted.id).first(), null);
    assert.equal(await env.PRESETS.get(row.r2_key), null);
    assert.ok(await env.DB.prepare("SELECT id FROM admin_audit WHERE action='auto-purge' AND preset_id=?").bind(submitted.id).first());
  });

  await t.test("concurrent approvals publish one revision and leave the loser recoverable", async () => {
    await env.DB.prepare("DELETE FROM client_whitelist WHERE submitter_hash=?").bind(await hashClientId(env, owner)).run();
    const submitted = await (await submitPreset(ownerRequest("POST", "/api/v1/presets", "Approval Race"), env)).json();
    let waiting = 0;
    let release;
    const barrier = new Promise((resolve) => { release = resolve; });
    const bucket = {
      get: (...args) => env.PRESETS.get(...args),
      head: (...args) => env.PRESETS.head(...args),
      put: async (...args) => {
        if (args[0].startsWith(`approved/${submitted.id}/`)) {
          waiting += 1;
          if (waiting === 2) release();
          await barrier;
        }
        return env.PRESETS.put(...args);
      }
    };
    const path = `/api/v1/admin/submissions/${submitted.id}/approve`;
    const results = await Promise.all([
      approveSubmission(adminRequest(path), { ...env, PRESETS: bucket }, submitted.id),
      approveSubmission(adminRequest(path), { ...env, PRESETS: bucket }, submitted.id)
    ]);
    assert.deepEqual(results.map((r) => r.status).sort(), [200, 409]);
    const active = await env.DB.prepare("SELECT status,r2_key FROM presets WHERE id=?").bind(submitted.id).first();
    assert.equal(active.status, "approved");
    assert.equal(await (await env.PRESETS.get(active.r2_key)).text(), ini);
    assert.equal((await env.DB.prepare("SELECT COUNT(*) AS n FROM preset_object_staging").first()).n, 1);
    await drainAbandonedPresetStaging(env, "9999-01-01T00:00:00.000Z");
    assert.ok(await env.PRESETS.get(active.r2_key));
  });

  await t.test("an existing fixed-key row remains editable after migration", async () => {
    const id = "legacy-fixed-key";
    const key = `approved/${id}/preset.ini`;
    const at = new Date().toISOString();
    const hash = await hashClientId(env, owner);
    await env.PRESETS.put(key, ini);
    await env.DB.prepare(
      `INSERT INTO presets (id,title,author_name,status,r2_key,sha256,size_bytes,format_version,min_addon_version,submitter_hash,safety_status,created_at,updated_at,approved_at)
       VALUES (?,?,?,'approved',?,?,?,?,?,?,?,?,?,?)`
    ).bind(id, "Legacy", "Author", key, await sha256Hex(ini), new TextEncoder().encode(ini).length, 6, "0.6.3",
      hash, "passed", at, at, at).run();
    await env.DB.prepare("INSERT INTO client_whitelist (submitter_hash,auto_approve,created_at,updated_at) VALUES (?,1,?,?)")
      .bind(hash, at, at).run();
    const response = await updateMyPreset(ownerRequest("PUT", `/api/v1/me/presets/${id}`, "Legacy Updated", ini.replace("0.2500", "0.5000")), env, id);
    assert.equal(response.status, 200);
    const updated = await env.DB.prepare("SELECT r2_key,content_revision FROM presets WHERE id=?").bind(id).first();
    assert.equal(updated.content_revision, 1);
    assert.notEqual(updated.r2_key, key);
    assert.equal(await (await env.PRESETS.get(updated.r2_key)).text(), ini.replace("0.2500", "0.5000"));
    assert.ok(await env.PRESETS.get(key));
    await drainPresetObjectCleanup(env);
    assert.equal(await env.PRESETS.get(key), null);
    assert.ok(await env.PRESETS.get(updated.r2_key));
  });
});
