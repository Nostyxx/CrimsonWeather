// Response helpers. Shapes and headers are part of the /api/v1 contract that
// shipped addon versions parse, so keep them byte-compatible.

export function json(body, status = 200, headers = {}) {
  return new Response(JSON.stringify(body), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", ...headers },
  });
}

export function text(body, status = 200, headers = {}) {
  return new Response(body, { status, headers: { "content-type": "text/plain; charset=utf-8", ...headers } });
}

// Error envelope clients display: { ok: false, error, details? }.
export function bad(message, status = 400, details = undefined) {
  return json({ ok: false, error: message, details }, status);
}

export const notFound = () => bad("Not found.", 404);

// Reads a request body into memory, stopping as soon as it passes maxBytes
// (a streamed body has no Content-Length to check up front). Null when too large.
export async function readBody(request, maxBytes) {
  if (Number(request.headers.get("content-length") || 0) > maxBytes) return null;
  if (!request.body) return new Uint8Array(0);
  const reader = request.body.getReader();
  const chunks = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > maxBytes) {
      await reader.cancel().catch(() => {});
      return null;
    }
    chunks.push(value);
  }
  const bytes = new Uint8Array(total);
  let offset = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, offset);
    offset += chunk.byteLength;
  }
  return bytes;
}

// Parses a JSON body; null when too large or malformed.
export async function readJson(request, maxBytes = 140000) {
  try {
    const bytes = await readBody(request, maxBytes);
    return bytes ? JSON.parse(new TextDecoder().decode(bytes)) : null;
  } catch {
    return null;
  }
}
