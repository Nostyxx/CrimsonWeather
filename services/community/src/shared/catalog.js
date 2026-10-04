// The public preset catalog.
//
// The catalog is a JSON document in R2 (catalog/catalog.v1.json), rewritten
// after every change that affects it and served as-is to the addon. Keeping it
// in R2 (rather than only building it on request) also means the original
// Worker would serve a current catalog if we ever rolled back.
import { json } from "./http.js";
import { nowIso } from "./values.js";

export const CATALOG_KEY = "catalog/catalog.v1.json";

// Approved, live presets (not pending updates, not deleted), most recently changed first.
async function approvedPresets(env) {
  const { results } = await env.DB.prepare(
    `SELECT id,title,author_name,description,tags_json,sha256,size_bytes,format_version,min_addon_version,
            downloads,likes,created_at,updated_at,approved_at
       FROM presets
      WHERE status='approved' AND update_of='' AND deleted_at IS NULL
      ORDER BY updated_at DESC`,
  ).all();
  return results || [];
}

function catalogFromRows(rows) {
  return {
    schemaVersion: 1,
    generatedAt: nowIso(),
    presets: rows.map((row) => ({
      id: row.id,
      title: row.title,
      author: row.author_name,
      description: row.description || "",
      tags: JSON.parse(row.tags_json || "[]"),
      formatVersion: row.format_version,
      minAddonVersion: row.min_addon_version,
      publishedAt: row.approved_at || row.created_at,
      updatedAt: row.updated_at,
      downloads: row.downloads || 0,
      likes: row.likes || 0,
      file: { url: `/api/v1/presets/${row.id}/download`, sha256: row.sha256, size: row.size_bytes },
      safety: { approved: true },
    })),
  };
}

async function nextGeneration(env) {
  const row = await env.DB.prepare(
    `INSERT INTO catalog_state (id,generation) VALUES (1,1)
     ON CONFLICT(id) DO UPDATE SET generation=catalog_state.generation+1
     RETURNING generation`,
  ).first();
  return Number(row.generation);
}

const generationOf = (object) => Number(object?.customMetadata?.generation || 0);

// The generation is also written into the file as trailing whitespace (valid
// JSON, invisible to parsers), so two publications never have the same ETag.
function serialize(catalog, generation) {
  const bits = generation.toString(2).replace(/[01]/g, (bit) => (bit === "0" ? "\t" : " "));
  return `${JSON.stringify(catalog, null, 2)}\n${bits}\n`;
}

// Regenerates the published catalog from the database.
//
// Rebuilds can overlap (several requests, both Workers). Each takes a
// generation after its own change is committed and reads the presets after
// that, so a higher generation always includes every earlier change. Only a
// higher generation may replace the published file.
export async function rebuildCatalog(env) {
  const generation = await nextGeneration(env);
  const catalog = catalogFromRows(await approvedPresets(env));
  const body = serialize(catalog, generation);
  for (let attempt = 0; attempt < 8; attempt++) {
    const current = await env.PRESETS.head(CATALOG_KEY);
    if (generationOf(current) >= generation) return catalog; // a newer rebuild already published
    const written = await env.PRESETS.put(CATALOG_KEY, body, {
      onlyIf: current ? { etagMatches: current.etag } : { etagDoesNotMatch: "*" },
      customMetadata: { generation: String(generation) },
      httpMetadata: { contentType: "application/json; charset=utf-8" },
    });
    if (written) return catalog;
  }
  throw new Error("Catalog publication kept changing; giving up on this rebuild.");
}

// GET /api/v1/catalog
export async function serveCatalog(env) {
  const published = await env.PRESETS.get(CATALOG_KEY);
  if (published) {
    return new Response(published.body, {
      headers: {
        "content-type": "application/json; charset=utf-8",
        "cache-control": `public, max-age=${Number(env.CATALOG_CACHE_SECONDS || 300)}`,
      },
    });
  }
  return json(await rebuildCatalog(env), 200, { "cache-control": "public, max-age=60" });
}
