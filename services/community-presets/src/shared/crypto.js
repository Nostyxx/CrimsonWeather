import { bytesOf } from "./values.js";

export async function sha256Hex(data) {
  const bytes = typeof data === "string" ? bytesOf(data) : data;
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

export async function hmacHex(secret, value) {
  if (!value) return "";
  const key = await crypto.subtle.importKey("raw", bytesOf(secret || "dev-secret"), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const sig = await crypto.subtle.sign("HMAC", key, bytesOf(value));
  return [...new Uint8Array(sig)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
