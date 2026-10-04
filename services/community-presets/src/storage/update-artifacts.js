import { UPDATE_ARTIFACT_MAX_BYTES } from "../updates/defaults.js";

const PART_BYTES = 5 * 1024 * 1024;

export class ArtifactValidationError extends Error {}

// Hash and store one bounded part at a time; never buffer the entire addon.
export async function storeUpdateArtifact(bucket, request, version) {
  const declaredLength = Number(request.headers.get("content-length") || 0);
  if (declaredLength > UPDATE_ARTIFACT_MAX_BYTES) throw new ArtifactValidationError("Addon file is too large.");
  if (!request.body) throw new ArtifactValidationError("Addon file is empty.");
  // Each attempt owns a unique immutable key, including concurrent same-version uploads.
  const key = `updates/${version}/artifacts/${crypto.randomUUID()}/CrimsonWeather.addon64`;
  const upload = await bucket.createMultipartUpload(key, { httpMetadata: { contentType: "application/octet-stream" } });
  const digest = new crypto.DigestStream("SHA-256");
  // Attach the rejection handler immediately, including early reader/R2 failures.
  const digestResult = digest.digest.then((value) => ({ value }), (error) => ({ error }));
  const hashWriter = digest.getWriter();
  const reader = request.body.getReader();
  const parts = [];
  let buffer = new Uint8Array(PART_BYTES);
  let used = 0;
  let size = 0;
  let completed = false;
  try {
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      size += value.byteLength;
      if (size > UPDATE_ARTIFACT_MAX_BYTES) throw new ArtifactValidationError("Addon file is too large.");
      await hashWriter.write(value);
      let offset = 0;
      while (offset < value.byteLength) {
        const count = Math.min(PART_BYTES - used, value.byteLength - offset);
        buffer.set(value.subarray(offset, offset + count), used);
        used += count;
        offset += count;
        if (used === PART_BYTES) {
          parts.push(await upload.uploadPart(parts.length + 1, buffer));
          buffer = new Uint8Array(PART_BYTES);
          used = 0;
        }
      }
    }
    if (!size) throw new ArtifactValidationError("Addon file is empty.");
    if (declaredLength > 0 && declaredLength !== size) throw new ArtifactValidationError("Addon file size does not match Content-Length.");
    if (used) parts.push(await upload.uploadPart(parts.length + 1, buffer.subarray(0, used)));
    await hashWriter.close();
    const result = await digestResult;
    if (result.error) throw result.error;
    const sha256 = [...new Uint8Array(result.value)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
    await upload.complete(parts);
    completed = true;
    return { key, sha256, size };
  } catch (error) {
    await Promise.allSettled([reader.cancel(error), hashWriter.abort(error)]);
    if (!completed) await upload.abort().catch(() => {});
    throw error;
  } finally {
    reader.releaseLock();
    hashWriter.releaseLock();
  }
}
