import { hmacHex } from "../shared/crypto.js";

export function deviceHashSecret(env) {
  return env.DEVICE_HASH_SECRET || env.ADMIN_TOKEN || "dev-secret";
}

export async function hashClientId(env, clientId) {
  return hmacHex(deviceHashSecret(env), clientId);
}

export function clientFingerprint(submitterHash) {
  return submitterHash ? `${submitterHash.slice(0, 12)}...${submitterHash.slice(-6)}` : "";
}

export function withClientFingerprint(row) {
  return { ...row, client_fingerprint: clientFingerprint(row?.submitter_hash || "") };
}

export async function submitterHashFromRequest(request, env) {
  const clientId = request.headers.get("x-cw-client-id") || "";
  if (!clientId || clientId.length > 128) return "";
  return hashClientId(env, clientId);
}
