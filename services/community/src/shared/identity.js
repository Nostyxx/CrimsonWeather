// Anonymous player identity.
//
// The addon generates a random id and sends it as `x-cw-client-id`. The server
// only ever stores HMAC(DEVICE_HASH_SECRET, id): that hash links a player to
// their uploads, likes and downloads. Changing the secret or this derivation
// would orphan every existing upload, so both are frozen.

const encoder = new TextEncoder();

function toHex(buffer) {
  return [...new Uint8Array(buffer)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

export async function sha256Hex(data) {
  const bytes = typeof data === "string" ? encoder.encode(data) : data;
  return toHex(await crypto.subtle.digest("SHA-256", bytes));
}

export async function hmacHex(secret, value) {
  if (!value) return "";
  const key = await crypto.subtle.importKey("raw", encoder.encode(secret), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  return toHex(await crypto.subtle.sign("HMAC", key, encoder.encode(value)));
}

// Both Workers must have the same secret as production. A missing secret is a
// configuration error: hashing with anything else would silently give every
// player a different identity, so fail instead.
function deviceHashSecret(env) {
  if (!env.DEVICE_HASH_SECRET) throw new Error("DEVICE_HASH_SECRET is not configured");
  return env.DEVICE_HASH_SECRET;
}

export const hashClientId = (env, clientId) => hmacHex(deviceHashSecret(env), clientId);

export const clientIdFrom = (request) => request.headers.get("x-cw-client-id") || "";

export const isValidClientId = (clientId) => !!clientId && clientId.length <= 128;

// Short, non-reversible display form of a submitter hash.
export function clientFingerprint(submitterHash) {
  return submitterHash ? `${submitterHash.slice(0, 12)}...${submitterHash.slice(-6)}` : "";
}
