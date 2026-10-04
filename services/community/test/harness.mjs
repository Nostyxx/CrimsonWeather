// Test harness: runs a Worker implementation in Miniflare against the production
// schema and records a normalized transcript of what clients observe.
import { readFileSync, readdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { Miniflare } from "miniflare";

const ROOT = join(dirname(fileURLToPath(import.meta.url)), "..");
export const ORIGIN = "https://cw.test";
export const ADMIN_ORIGIN = "https://admin.cw.test";

// Fixed secrets so hashed client identities are identical across implementations.
export const TEST_ENV = {
  ADMIN_TOKEN: "test-admin-token",
  ADMIN_LOGIN_KEY: "test-login-key",
  DEVICE_HASH_SECRET: "test-device-secret",
  CATALOG_CACHE_SECONDS: "300",
  MAX_PRESET_BYTES: "65536",
  MIN_ADDON_VERSION: "0.6.3",
};

// Only the migrations production has applied; newer ones belong to the new Worker.
const LEGACY_MIGRATIONS = [
  "0001_initial.sql",
  "0002_whitelist_and_owner_updates.sql",
  "0003_admin_soft_delete.sql",
  "0004_update_settings.sql",
];

function splitSql(sql) {
  return sql.split(/;\s*(?:\r?\n|$)/).map((s) => s.trim()).filter(Boolean);
}

export function migrationFiles(all) {
  const files = readdirSync(join(ROOT, "migrations")).filter((f) => f.endsWith(".sql")).sort();
  return all ? files : files.filter((f) => LEGACY_MIGRATIONS.includes(f));
}

// Starts one implementation. Legacy is a single Worker; the rewrite is a public
// Worker plus an admin Worker sharing the same D1 database and R2 bucket.
export async function startWorker({ scriptPath, adminScriptPath, allMigrations = false, bindings = {}, adminBindings = {}, adminOutbound }) {
  const shared = {
    modules: true,
    compatibilityDate: "2026-05-20",
    d1Databases: { DB: "community-db" },
    r2Buckets: { PRESETS: "community-bucket" },
  };
  const workers = [{ name: "public", ...shared, script: readFileSync(scriptPath, "utf8"), bindings: { ...TEST_ENV, ...bindings } }];
  if (adminScriptPath) {
    workers.push({ name: "admin", ...shared, script: readFileSync(adminScriptPath, "utf8"),
      bindings: { ...adminBindings }, outboundService: adminOutbound });
  }
  const mf = new Miniflare({ workers });
  const db = await mf.getD1Database("DB", "public");
  for (const file of migrationFiles(allMigrations)) {
    for (const stmt of splitSql(readFileSync(join(ROOT, "migrations", file), "utf8"))) {
      await db.prepare(stmt).run();
    }
  }
  const r2 = await mf.getR2Bucket("PRESETS", "public");
  const admin = adminScriptPath ? await mf.getWorker("admin") : null;
  return {
    mf,
    db,
    r2,
    fetch: (path, init) => mf.dispatchFetch(ORIGIN + path, init),
    adminFetch: (path, init) => admin.fetch(ADMIN_ORIGIN + path, init),
    scheduled: async () => (await mf.getWorker("public")).scheduled({ cron: "17 3 * * *" }),
    dispose: () => mf.dispose(),
  };
}

// ---------------------------------------------------------------------------
// Normalization: replace values that legitimately differ between runs.
// ---------------------------------------------------------------------------
export class Normalizer {
  constructor() {
    this.hex = new Map(); // random id suffix -> stable token
  }

  token(hex) {
    if (!this.hex.has(hex)) this.hex.set(hex, `<h${this.hex.size + 1}>`);
    return this.hex.get(hex);
  }

  string(value) {
    return value
      // R2 keys: the file name inside a preset's or release's folder is an
      // implementation detail (the original Worker uses fixed names, the
      // rewrite one name per revision); what matters is that the referenced
      // file exists with the right hash, which the snapshot checks.
      .replace(/^(pending|approved)\/([^/]+)\/[^/]+\.ini$/, "$1/$2/<file>")
      .replace(/^updates\/([^/]+)\/[^/]+$/, "updates/$1/<file>")
      // generated preset ids: <slug>-<8 hex> and <slug>-update-<8 hex>
      .replace(/(^|[^0-9a-f])([0-9a-f]{8})(?![0-9a-f])/g, (m, pre, hex) =>
        /-$/.test(pre) || pre === "" ? pre + this.token(hex) : m)
      .replace(/\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{3})?Z/g, "<ts>")
      .replace(/^\d{4}-\d{2}-\d{2}$/, "<day>");
  }

  value(v) {
    if (typeof v === "string") return this.string(v);
    if (Array.isArray(v)) return v.map((x) => this.value(x));
    if (v && typeof v === "object") {
      const out = {};
      for (const [k, x] of Object.entries(v)) out[k] = this.value(x);
      return out;
    }
    return v;
  }
}

const RECORDED_HEADERS = [
  "content-type",
  "cache-control",
  "content-disposition",
  "x-cw-addon-sha256",
  "x-cw-addon-size-bytes",
];

// ---------------------------------------------------------------------------
// Scenario context: what a scenario can do, and what gets recorded.
// ---------------------------------------------------------------------------
export class Recorder {
  constructor(worker, admin) {
    this.worker = worker;
    this.admin = admin;
    this.norm = new Normalizer();
    this.steps = [];
    this.lastJson = null;
  }

  // Performs a client request and records the observable response.
  async call(label, method, path, { client, clientVersion, json, body, headers = {} } = {}) {
    await sleep(4); // keep timestamps strictly increasing between steps
    const h = { ...headers };
    if (client) h["x-cw-client-id"] = client;
    if (clientVersion) h["x-cw-client-version"] = clientVersion;
    let payload = body;
    if (json !== undefined) {
      h["content-type"] = "application/json";
      payload = JSON.stringify(json);
    }
    const res = await this.worker.fetch(path, { method, headers: h, body: payload });
    const raw = await res.text();
    let parsed = raw;
    try { parsed = JSON.parse(raw); } catch { /* text body */ }
    this.lastJson = typeof parsed === "object" ? parsed : null;
    const recordedHeaders = {};
    for (const name of RECORDED_HEADERS) {
      const value = res.headers.get(name);
      if (value !== null) recordedHeaders[name] = value;
    }
    this.steps.push(this.norm.value({ step: label, request: `${method} ${path}`, status: res.status,
      headers: recordedHeaders, body: parsed }));
    return this.lastJson;
  }

  // Records the persistent state clients depend on (and a rollback would inherit).
  async snapshot(label) {
    const q = async (sql) => (await this.worker.db.prepare(sql).all()).results;
    const presets = await q(`SELECT id,title,author_name,description,tags_json,status,r2_key,sha256,size_bytes,
      format_version,min_addon_version,submitter_hash,safety_status,safety_summary,downloads,likes,created_at,
      updated_at,approved_at,rejected_at,update_of,deleted_at,delete_after,deleted_by,delete_reason FROM presets`);
    const likes = await q("SELECT preset_id,device_hash FROM preset_likes");
    const downloads = await q("SELECT preset_id,device_hash,day FROM preset_downloads_daily");
    const whitelist = await q("SELECT submitter_hash,label,auto_approve,note FROM client_whitelist");
    const settings = await q("SELECT key,value FROM app_settings");
    // R2: only what the data references must be intact (stray copies are an
    // implementation detail). Each referenced file must exist with the hash the
    // database claims; the published catalog is compared by content.
    const sha = async (key) => {
      const obj = await this.worker.r2.get(key);
      if (!obj) return null;
      return Buffer.from(await crypto.subtle.digest("SHA-256", await obj.arrayBuffer())).toString("hex");
    };
    const files = [];
    for (const p of presets) {
      const actual = await sha(p.r2_key);
      files.push({ key: p.r2_key, state: actual === null ? "missing" : actual === p.sha256 ? "ok" : "hash-mismatch" });
    }
    const artifactKey = settings.find((s) => s.key === "update.addonR2Key")?.value;
    if (artifactKey) {
      const expected = settings.find((s) => s.key === "update.addonSha256")?.value;
      const actual = await sha(artifactKey);
      files.push({ key: artifactKey, state: actual === null ? "missing" : actual === expected ? "ok" : "hash-mismatch" });
    }
    const catalogObj = await this.worker.r2.get("catalog/catalog.v1.json");
    const catalog = catalogObj ? JSON.parse(await catalogObj.text()) : null;

    const sortBy = (rows, key) => [...rows].sort((a, b) => String(a[key]).localeCompare(String(b[key])));
    const state = this.norm.value({
      presets: presets.map((p) => ({ ...p, deleted_by: p.deleted_by.startsWith("admin") ? "<admin>" : p.deleted_by })),
      likes, downloads, whitelist, settings, files, catalog,
    });
    state.presets = sortBy(state.presets, "id");
    state.likes = sortBy(state.likes, "preset_id");
    state.downloads = sortBy(state.downloads, "preset_id");
    state.settings = sortBy(state.settings, "key");
    state.files = sortBy(state.files, "key");
    this.steps.push({ step: label, state });
  }

  transcript() {
    return this.steps;
  }
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
