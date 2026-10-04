import { json } from "../http/responses.js";
import { getApprovedPresetRows, nextCatalogGeneration } from "../storage/presets-repository.js";
import { getCatalogObject, putCatalogIfNewer } from "../storage/objects.js";

const nowIso = () => new Date().toISOString();

export function catalogFromRows(rows, generatedAt = nowIso()) {
  return {
    schemaVersion: 1,
    generatedAt,
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
      file: {
        url: `/api/v1/presets/${row.id}/download`,
        sha256: row.sha256,
        size: row.size_bytes
      },
      safety: {
        approved: true
      }
    }))
  };
}

export function serializeCatalog(catalog, generation) {
  // JSON permits trailing spaces and tabs. Encoding the generation there keeps
  // the public object shape stable while ensuring R2's content ETag changes
  // even when catalog data and generatedAt happen to be identical.
  const generationBits = generation.toString(2).replace(/[01]/g, (bit) => bit === "0" ? "\t" : " ");
  return `${JSON.stringify(catalog, null, 2)}\n${generationBits}\n`;
}

export async function rebuildCatalog(env) {
  const generation = await nextCatalogGeneration(env, nowIso());
  const catalog = catalogFromRows(await getApprovedPresetRows(env));
  const publication = await putCatalogIfNewer(env, generation, serializeCatalog(catalog, generation));
  if (publication.published) return catalog;
  const current = await getCatalogObject(env);
  if (current?.body) {
    try {
      return JSON.parse(await current.text());
    } catch {
      throw new Error("The current catalog object is invalid; the previous catalog was preserved.");
    }
  }
  throw new Error("Catalog rebuild was superseded and no catalog object is available.");
}

export async function getCatalog(env) {
  const cached = await getCatalogObject(env);
  if (cached) {
    return new Response(cached.body, {
      headers: {
        "content-type": "application/json; charset=utf-8",
        "cache-control": `public, max-age=${Number(env.CATALOG_CACHE_SECONDS || 300)}`
      }
    });
  }
  const catalog = await rebuildCatalog(env);
  return json(catalog, 200, { "cache-control": "public, max-age=60" });
}
