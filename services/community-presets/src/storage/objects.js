export const CATALOG_KEY = "catalog/catalog.v1.json";

export function catalogGenerationOf(object) {
  const generation = Number(object?.customMetadata?.catalogGeneration || 0);
  return Number.isSafeInteger(generation) && generation >= 0 ? generation : 0;
}

export async function getCatalogObject(env) {
  return env.PRESETS.get(CATALOG_KEY);
}

export async function putCatalogIfNewer(env, generation, serializedCatalog) {
  for (let attempt = 0; attempt < 8; attempt += 1) {
    const current = await env.PRESETS.head(CATALOG_KEY);
    if (catalogGenerationOf(current) >= generation) {
      return { published: false };
    }

    const onlyIf = current
      ? { etagMatches: current.etag }
      : { etagDoesNotMatch: "*" };
    const object = await env.PRESETS.put(CATALOG_KEY, serializedCatalog, {
      onlyIf,
      customMetadata: { catalogGeneration: String(generation) },
      httpMetadata: { contentType: "application/json; charset=utf-8" }
    });
    if (object) return { published: true };
  }
  throw new Error("Catalog publication kept changing; retry the rebuild.");
}
