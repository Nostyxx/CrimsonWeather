import { getUpdateSettings } from "./settings.js";
import { normalizeVersion } from "../shared/version.js";
import { bad, json } from "../http/responses.js";
import { publishUpdateSettings } from "../storage/settings-repository.js";
import { ArtifactValidationError, storeUpdateArtifact } from "../storage/update-artifacts.js";

export async function updateArtifact(request, env) {
  const url = new URL(request.url);
  const settings = await getUpdateSettings(env);
  const requestedVersion = normalizeVersion(url.searchParams.get("version") || "");
  if (requestedVersion && requestedVersion !== settings.latestVersion) {
    return bad("Requested update artifact version is not available.", 404);
  }
  if (!settings.addonR2Key || !/^[a-f0-9]{64}$/i.test(settings.addonSha256)) {
    return bad("Update artifact is not configured.", 404);
  }
  const object = await env.PRESETS.get(settings.addonR2Key);
  if (!object) return bad("Update artifact was not found.", 404);
  return new Response(object.body, {
    status: 200,
    headers: {
      "content-type": "application/octet-stream",
      "content-disposition": `attachment; filename="CrimsonWeather-${settings.latestVersion}.addon64"`,
      "x-cw-addon-sha256": settings.addonSha256,
      "x-cw-addon-size-bytes": String(settings.addonSizeBytes || ""),
      "cache-control": "no-store"
    }
  });
}

export async function adminUploadUpdateArtifact(request, env) {
  const url = new URL(request.url);
  const version = normalizeVersion(url.searchParams.get("version") || request.headers.get("x-cw-update-version") || "");
  if (!/^\d+(?:\.\d+){1,3}$/.test(version)) return bad("Version must look like 0.6.6.");
  let artifact;
  try {
    artifact = await storeUpdateArtifact(env.PRESETS, request, version);
  } catch (error) {
    if (error instanceof ArtifactValidationError) return bad(error.message);
    throw error;
  }
  const { key: r2Key, sha256, size } = artifact;
  await publishUpdateSettings(env, request, {
    "update.latestVersion": version,
    "update.addonR2Key": r2Key,
    "update.addonSha256": sha256,
    "update.addonSizeBytes": String(size)
  }, "update-artifact", `version=${version} sha256=${sha256}`);
  return json({
    ok: true,
    version,
    addonR2Key: r2Key,
    addonSha256: sha256,
    addonSizeBytes: size
  }, 200, { "cache-control": "no-store" });
}
