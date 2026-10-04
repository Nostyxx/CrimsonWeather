// Regression tests for overlapping requests and failures part-way through a
// change (findings 1-7 of docs/reviews/2026-10-01-backend-rewrite-review.md).
//
// The handlers from src/ run in Node against Miniflare's D1 and R2. Their
// storage calls go through gates that can hold one call until the test lets it
// continue, or make it fail, so each interleaving is exact, not timing-based.
import { test } from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { startWorker, TEST_ENV } from "./harness.mjs";
import { IMPLEMENTATIONS } from "./run.mjs";
import { PRESET } from "./scenarios.mjs";
import * as pub from "../src/public/presets.js";
import { updateArtifact } from "../src/public/updates.js";
import * as lifecycle from "../src/shared/presets.js";
import { rebuildCatalog, CATALOG_KEY } from "../src/shared/catalog.js";
import { uploadArtifact } from "../src/admin/api.js";

const ALICE = "client-alice-0001";
const ADMIN = "admin:test@example.com";
const PRESET_V2 = PRESET.replace("Rain=0.5", "Rain=0.75");
const PRESET_V3 = PRESET.replace("Rain=0.5", "Rain=0.9");
const sha = (data) => createHash("sha256").update(data).digest("hex");

// ---------------------------------------------------------------------------
// Gates: hold or fail the first storage call that matches.
// ---------------------------------------------------------------------------
class Gates {
  constructor() {
    this.list = [];
  }

  // Holds the first matching call. `reached` resolves when it is held.
  hold(match) {
    const gate = { match, mode: "hold" };
    gate.reached = new Promise((resolve) => { gate.onReached = resolve; });
    gate.released = new Promise((resolve) => { gate.release = resolve; });
    this.list.push(gate);
    return gate;
  }

  // Makes the first matching call throw.
  fail(match) {
    this.list.push({ match, mode: "fail" });
  }

  async pass(op, detail) {
    for (const gate of this.list) {
      if (gate.used || !gate.match(op, detail)) continue;
      gate.used = true;
      if (gate.mode === "fail") throw new Error(`injected failure at ${op}`);
      gate.onReached();
      await gate.released;
    }
  }
}

function gated(env, gates) {
  const r2 = env.PRESETS;
  const db = env.DB;
  const statement = (inner, sql) => ({
    inner,
    sql,
    bind: (...values) => statement(inner.bind(...values), sql),
    run: async () => { await gates.pass("db.run", sql); return inner.run(); },
    first: async (column) => { await gates.pass("db.first", sql); return column === undefined ? inner.first() : inner.first(column); },
    all: async () => { await gates.pass("db.all", sql); return inner.all(); },
  });
  return {
    ...env,
    DB: {
      prepare: (sql) => statement(db.prepare(sql), sql),
      batch: async (list) => { await gates.pass("db.batch", list.map((s) => s.sql).join("\n")); return db.batch(list.map((s) => s.inner)); },
    },
    PRESETS: {
      put: async (key, ...rest) => { await gates.pass("r2.put", key); return r2.put(key, ...rest); },
      get: async (key, ...rest) => { await gates.pass("r2.get", key); return r2.get(key, ...rest); },
      head: async (key) => { await gates.pass("r2.head", key); return r2.head(key); },
      delete: async (keys) => { await gates.pass("r2.delete", keys); return r2.delete(keys); },
      list: (options) => r2.list(options),
    },
  };
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------
async function setup() {
  const worker = await startWorker(IMPLEMENTATIONS.next);
  const env = { ...TEST_ENV, DB: worker.db, PRESETS: worker.r2 };
  const gates = new Gates();
  return { worker, env, gates, genv: gated(env, gates) };
}

function request(method, path, { client, json, body, headers = {} } = {}) {
  const h = { ...headers };
  if (client) h["x-cw-client-id"] = client;
  if (json !== undefined) h["content-type"] = "application/json";
  return new Request(`https://cw.test${path}`, {
    method,
    headers: h,
    body: json !== undefined ? JSON.stringify(json) : body,
    ...(body instanceof ReadableStream ? { duplex: "half" } : {}),
  });
}

const read = async (response) => ({ status: response.status, body: await response.json() });

async function submit(env, iniText = PRESET, client = ALICE) {
  const res = await read(await pub.submitPreset(request("POST", "/api/v1/presets", {
    client, json: { title: "Clean Morning", authorName: "Alice", iniText, clientVersion: "0.8.1" },
  }), env));
  assert.equal(res.status, 200, JSON.stringify(res.body));
  return res.body.id;
}

async function submitApproved(env) {
  const id = await submit(env);
  assert.equal((await lifecycle.approve(env, ADMIN, id)).ok, true);
  return id;
}

const edit = (env, id, iniText, client = ALICE) =>
  pub.updateMyPreset(request("PUT", `/api/v1/me/presets/${id}`, { client, json: { title: "Clean Morning", authorName: "Alice", iniText } }), env, id).then(read);

const row = (env, id) => env.DB.prepare("SELECT * FROM presets WHERE id=?").bind(id).first();

async function catalogEntry(env, id) {
  const object = await env.PRESETS.get(CATALOG_KEY);
  return JSON.parse(await object.text()).presets.find((p) => p.id === id);
}

async function downloadText(env, id) {
  const res = await pub.downloadPreset(request("GET", `/api/v1/presets/${id}/download`), env, id);
  assert.equal(res.status, 200);
  return res.text();
}

// What a player gets must be consistent: the catalog's hash is the hash of
// the file the download serves.
async function assertServed(env, id, iniText) {
  const text = await downloadText(env, id);
  assert.equal(text, iniText);
  assert.equal((await catalogEntry(env, id))?.file.sha256, sha(iniText));
}

async function withSetup(fn) {
  const ctx = await setup();
  try {
    await fn(ctx);
  } finally {
    await ctx.worker.dispose();
  }
}

// ---------------------------------------------------------------------------
// 1. Owner edit overlapping approval
// ---------------------------------------------------------------------------
test("an owner edit racing an approval cannot publish unreviewed content", () => withSetup(async ({ env, gates, genv }) => {
  const id = await submit(env);
  const gate = gates.hold((op, key) => op === "r2.put" && key.startsWith(`pending/${id}/`));
  const editing = edit(genv, id, PRESET_V2);
  await gate.reached; // the edit has read the pending row
  assert.equal((await lifecycle.approve(env, ADMIN, id, sha(PRESET))).ok, true);
  gate.release();

  const res = await editing;
  assert.equal(res.status, 409);
  const after = await row(env, id);
  assert.equal(after.status, "approved");
  assert.equal(after.sha256, sha(PRESET));
  await assertServed(env, id, PRESET);
}));

test("approval only applies to the content the moderator reviewed", () => withSetup(async ({ env }) => {
  const id = await submit(env);
  assert.equal((await edit(env, id, PRESET_V2)).status, 200); // edited after the moderator opened it
  const res = await lifecycle.approve(env, ADMIN, id, sha(PRESET));
  assert.equal(res.status, 409);
  assert.equal((await row(env, id)).status, "pending");
  assert.equal((await lifecycle.approve(env, ADMIN, id, sha(PRESET_V2))).ok, true);
  await assertServed(env, id, PRESET_V2);
}));

// ---------------------------------------------------------------------------
// 2. Failures part-way through replacing published files
// ---------------------------------------------------------------------------
test("a failed artifact re-upload leaves the published file intact", () => withSetup(async ({ env, gates, genv }) => {
  const upload = (e, bytes) => uploadArtifact(request("PUT", "/api/admin/release/artifact?version=0.8.2", { body: bytes }), e, ADMIN);
  const first = new TextEncoder().encode("addon build one");
  const second = new TextEncoder().encode("addon build two, different");
  assert.equal((await upload(env, first)).status, 200);

  gates.fail((op, sql) => op === "db.batch" && sql.includes("app_settings"));
  await assert.rejects(upload(genv, second));
  const served = await updateArtifact(request("GET", "/api/v1/update/artifact?version=0.8.2"), env);
  const bytes = new Uint8Array(await served.arrayBuffer());
  assert.equal(served.headers.get("x-cw-addon-sha256"), sha(first));
  assert.equal(sha(bytes), sha(first));

  assert.equal((await upload(env, second)).status, 200);
  const replaced = await updateArtifact(request("GET", "/api/v1/update/artifact?version=0.8.2"), env);
  assert.equal(sha(new Uint8Array(await replaced.arrayBuffer())), sha(second));
  const { objects } = await env.PRESETS.list({ prefix: "updates/0.8.2/" });
  assert.equal(objects.length, 1, "the replaced file is removed once nothing points at it");
}));

test("a failed update approval leaves the live preset intact", () => withSetup(async ({ env, gates, genv }) => {
  const id = await submitApproved(env);
  const res = await edit(env, id, PRESET_V2);
  const updateId = res.body.updateId;
  gates.fail((op, sql) => op === "db.batch" && sql.includes("u.status='pending'"));
  await assert.rejects(lifecycle.approve(genv, ADMIN, updateId));
  await assertServed(env, id, PRESET);
  assert.equal((await row(env, updateId)).status, "pending");

  assert.equal((await lifecycle.approve(env, ADMIN, updateId)).ok, true);
  await assertServed(env, id, PRESET_V2);
  const { objects } = await env.PRESETS.list({ prefix: `approved/${id}/` });
  assert.equal(objects.length, 1, "the previous live file is removed after the switch");
}));

// ---------------------------------------------------------------------------
// 3. Scheduled purge overlapping a restore
// ---------------------------------------------------------------------------
async function expire(env, id) {
  await env.DB.prepare("UPDATE presets SET delete_after='2000-01-01T00:00:00.000Z' WHERE id=?").bind(id).run();
}

test("a restore during the scheduled purge wins", () => withSetup(async ({ env, gates, genv }) => {
  const id = await submitApproved(env);
  assert.equal((await lifecycle.softDelete(env, ADMIN, id, "test")).ok, true);
  await expire(env, id);
  const gate = gates.hold((op, sql) => op === "db.run" && sql.startsWith("DELETE FROM presets"));
  const purging = lifecycle.purgeExpiredPresets(genv);
  await gate.reached; // the purge has selected the row
  assert.equal((await lifecycle.restore(env, ADMIN, id)).ok, true);
  gate.release();

  assert.deepEqual(await purging, { purged: 0 });
  assert.equal((await row(env, id)).deleted_at, null);
  await assertServed(env, id, PRESET);
}));

test("a restore after the purge claimed the preset reports it gone", () => withSetup(async ({ env }) => {
  const id = await submitApproved(env);
  await lifecycle.softDelete(env, ADMIN, id, "test");
  await expire(env, id);
  assert.deepEqual(await lifecycle.purgeExpiredPresets(env), { purged: 1 });
  assert.equal((await lifecycle.restore(env, ADMIN, id)).status, 404);
  const { objects } = await env.PRESETS.list({ prefix: `approved/${id}/` });
  assert.equal(objects.length, 0);
}));

// ---------------------------------------------------------------------------
// 4. Overlapping catalog rebuilds
// ---------------------------------------------------------------------------
test("an older catalog rebuild never overwrites a newer one", () => withSetup(async ({ env, gates, genv }) => {
  const id = await submitApproved(env);
  const gate = gates.hold((op, key) => op === "r2.put" && key === CATALOG_KEY);
  const older = rebuildCatalog(genv);
  await gate.reached; // read the presets while `id` was still live
  assert.equal((await lifecycle.softDelete(env, ADMIN, id, "test")).ok, true); // publishes a newer catalog
  gate.release();
  await older;
  assert.equal(await catalogEntry(env, id), undefined);
}));

// ---------------------------------------------------------------------------
// 5. Simultaneous owner updates
// ---------------------------------------------------------------------------
test("of two simultaneous owner updates exactly the newest stays pending", () => withSetup(async ({ env, gates, genv }) => {
  const id = await submitApproved(env);
  const gate = gates.hold((op, sql) => op === "db.batch" && sql.includes("INSERT INTO presets"));
  const first = edit(genv, id, PRESET_V2);
  await gate.reached;
  const second = await edit(env, id, PRESET_V3);
  gate.release();
  const firstRes = await first;

  assert.equal(second.status, 200);
  assert.equal(firstRes.status, 409);
  const { results } = await env.DB.prepare("SELECT id,sha256 FROM presets WHERE update_of=? AND status='pending' AND deleted_at IS NULL").bind(id).all();
  assert.deepEqual(results.map((r) => r.id), [second.body.updateId]);
  assert.equal(results[0].sha256, sha(PRESET_V3));
}));

// ---------------------------------------------------------------------------
// 6. Like and download counters
// ---------------------------------------------------------------------------
test("like counters match the like records under overlapping toggles", () => withSetup(async ({ env, gates, genv }) => {
  const id = await submitApproved(env);
  const like = (e) => pub.toggleLike(request("POST", `/api/v1/presets/${id}/like`, { client: ALICE }), e, id).then(read);

  // Held after finding no like, before adding one; meanwhile the same player likes.
  const gate = gates.hold((op, sql) => op === "db.batch" && sql.includes("INSERT OR IGNORE INTO preset_likes"));
  const held = like(genv);
  await gate.reached;
  assert.equal((await like(env)).body.likes, 1);
  gate.release();
  await held;

  await Promise.all(Array.from({ length: 9 }, () => like(env)));
  const counted = await env.DB.prepare("SELECT likes, (SELECT COUNT(*) FROM preset_likes WHERE preset_id=?) AS records FROM presets WHERE id=?").bind(id, id).first();
  assert.equal(counted.likes, counted.records);
}));

test("overlapping downloads by one player count once a day", () => withSetup(async ({ env }) => {
  const id = await submitApproved(env);
  await Promise.all(Array.from({ length: 8 }, () => pub.downloadPreset(request("GET", `/api/v1/presets/${id}/download`, { client: ALICE }), env, id).then((r) => r.text())));
  assert.equal((await row(env, id)).downloads, 1);
}));

// ---------------------------------------------------------------------------
// 7. Request size and configuration
// ---------------------------------------------------------------------------
test("a streamed body without Content-Length is still size-limited", () => withSetup(async ({ env }) => {
  const chunk = new TextEncoder().encode("x".repeat(10000));
  let sent = 0;
  const stream = new ReadableStream({
    pull(controller) {
      if (sent === 0) controller.enqueue(new TextEncoder().encode('{"title":"t","iniText":"[CrimsonWeatherPreset]","pad":"'));
      if (sent >= 20) {
        controller.enqueue(new TextEncoder().encode('"}'));
        controller.close();
        return;
      }
      controller.enqueue(chunk);
      sent++;
    },
  });
  const req = request("POST", "/api/v1/presets", { client: ALICE, body: stream, headers: { "content-type": "application/json" } });
  assert.equal(req.headers.get("content-length"), null);
  const res = await read(await pub.submitPreset(req, env));
  assert.equal(res.status, 400);
  assert.equal(res.body.error, "Invalid JSON.");
}));

test("a Worker without the device secret refuses instead of hashing with a fallback", () => withSetup(async ({ env }) => {
  const { DEVICE_HASH_SECRET, ...withoutSecret } = env;
  await assert.rejects(submit(withoutSecret), /DEVICE_HASH_SECRET/);
}));
