import test from "node:test";
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { serializeCatalog } from "../src/presets/catalog.js";
import { putCatalogIfNewer } from "../src/storage/objects.js";

const etag = (body) => createHash("md5").update(body).digest("hex");
const catalog = { schemaVersion: 1, generatedAt: "2026-01-01T00:00:00.000Z", presets: [] };

test("generation serialization preserves the public JSON shape and gives identical catalogs distinct ETags", () => {
  const first = serializeCatalog(catalog, 1);
  const second = serializeCatalog(catalog, 2);
  assert.deepEqual(JSON.parse(first), catalog);
  assert.deepEqual(JSON.parse(second), catalog);
  assert.notEqual(etag(first), etag(second));
});

test("a delayed older publication cannot overwrite a newer identical catalog", async () => {
  const initial = serializeCatalog(catalog, 0);
  let current = { etag: etag(initial), body: initial, customMetadata: { catalogGeneration: "0" } };
  let reached;
  let release;
  const blocked = new Promise((resolve) => { reached = resolve; });
  const resume = new Promise((resolve) => { release = resolve; });
  const env = { PRESETS: {
    async head() { return { ...current, customMetadata: { ...current.customMetadata } }; },
    async put(_key, body, options) {
      if (options.customMetadata.catalogGeneration === "1") { reached(); await resume; }
      if (options.onlyIf.etagMatches !== current.etag) return null;
      current = { etag: etag(body), body, customMetadata: options.customMetadata };
      return current;
    }
  } };
  const old = putCatalogIfNewer(env, 1, serializeCatalog({ ...catalog, presets: [{ id: "old" }] }, 1));
  await blocked;
  try {
    assert.equal((await putCatalogIfNewer(env, 2, serializeCatalog(catalog, 2))).published, true);
  } finally { release(); }
  assert.equal((await old).published, false);
  assert.equal(current.customMetadata.catalogGeneration, "2");
  assert.deepEqual(JSON.parse(current.body), catalog);
});

test("failed R2 publication leaves the previous catalog intact", async () => {
  const body = serializeCatalog(catalog, 4);
  const current = { etag: etag(body), body, customMetadata: { catalogGeneration: "4" } };
  const env = { PRESETS: {
    async head() { return current; },
    async put() { throw new Error("injected storage failure"); }
  } };
  await assert.rejects(putCatalogIfNewer(env, 5, serializeCatalog(catalog, 5)), /injected storage failure/);
  assert.equal(current.body, body);
  assert.equal(current.customMetadata.catalogGeneration, "4");
});

test("continuous conditional-write conflicts fail after bounded retries", async () => {
  let attempts = 0;
  const env = { PRESETS: {
    async head() { return { etag: "changed", customMetadata: { catalogGeneration: "0" } }; },
    async put() { attempts += 1; return null; }
  } };
  await assert.rejects(putCatalogIfNewer(env, 1, serializeCatalog(catalog, 1)), /retry the rebuild/);
  assert.equal(attempts, 8);
});
