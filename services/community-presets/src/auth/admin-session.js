import { hmacHex } from "../shared/crypto.js";
import { readJson } from "../http/request-validation.js";
import { bad, json, text } from "../http/responses.js";

export function cookieValue(request, name) {
  const cookie = request.headers.get("cookie") || "";
  for (const part of cookie.split(";")) {
    const [rawKey, ...rawValue] = part.trim().split("=");
    if (rawKey === name) return rawValue.join("=");
  }
  return "";
}

export function base64UrlEncode(value) {
  return btoa(value).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/g, "");
}

export function base64UrlDecode(value) {
  const padded = value.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((value.length + 3) % 4);
  return atob(padded);
}

export async function makeAdminSession(env) {
  const payload = base64UrlEncode(JSON.stringify({ exp: Date.now() + 24 * 60 * 60 * 1000 }));
  const sig = await hmacHex(env.ADMIN_TOKEN, payload);
  return `${payload}.${sig}`;
}

export async function hasValidAdminSession(request, env) {
  if (!env.ADMIN_TOKEN) return false;
  const session = cookieValue(request, "cw_admin_session");
  const [payload, sig] = session.split(".");
  if (!payload || !sig) return false;
  const expected = await hmacHex(env.ADMIN_TOKEN, payload);
  if (sig !== expected) return false;
  try {
    const data = JSON.parse(base64UrlDecode(payload));
    return Number(data.exp || 0) > Date.now();
  } catch {
    return false;
  }
}

export async function isAdmin(request, env) {
  const auth = request.headers.get("authorization") || "";
  const token = env.ADMIN_TOKEN || "";
  if (token && auth === `Bearer ${token}`) return true;
  if (request.headers.get("cf-access-authenticated-user-email")) return true;
  return hasValidAdminSession(request, env);
}

export function hasAdminLoginKey(request, env, body = undefined) {
  const expected = String(env.ADMIN_LOGIN_KEY || "");
  if (!expected) return true;
  const url = new URL(request.url);
  const supplied =
    url.searchParams.get("key") ||
    request.headers.get("x-cw-admin-login-key") ||
    (body && body.loginKey) ||
    "";
  return String(supplied) === expected;
}

export function adminIdentity(request, env) {
  if (env.ADMIN_TOKEN && (request.headers.get("authorization") || "") === `Bearer ${env.ADMIN_TOKEN}`) return "admin-token";
  return request.headers.get("cf-access-authenticated-user-email") || "admin-session";
}

export async function loginAdmin(request, env) {
  const body = await readJson(request);
  if (!hasAdminLoginKey(request, env, body)) return text("Not found.", 404);
  if (!env.ADMIN_TOKEN || !body || body.token !== env.ADMIN_TOKEN) {
    return bad("Invalid admin token.", 401);
  }
  const session = await makeAdminSession(env);
  return json({ ok: true }, 200, {
    "set-cookie": `cw_admin_session=${session}; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=86400`
  });
}

export function logoutAdmin() {
  return json({ ok: true }, 200, {
    "set-cookie": "cw_admin_session=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0"
  });
}
