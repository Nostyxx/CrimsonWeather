// Admin Worker: moderation and release management.
//
// Deployed on its own hostname behind Cloudflare Access; every request is
// re-authenticated here (see access.js). Shares the D1 database and R2 bucket
// with the public Worker.
import { bad, json, notFound, text } from "../shared/http.js";
import { authenticate } from "./access.js";
import * as api from "./api.js";
import { ASSETS } from "./page.js";
import { isAllowedOrigin } from "./origin.js";

const ROUTES = [
  ["GET", /^\/api\/admin\/me$/, (req, env, actor) => json({ ok: true, actor })],
  ["GET", /^\/api\/admin\/overview$/, (req, env) => api.overview(env)],
  ["GET", /^\/api\/admin\/presets$/, (req, env) => api.listPresets(env, new URL(req.url).searchParams)],
  ["POST", /^\/api\/admin\/presets\/bulk$/, (req, env, actor) => api.bulkAction(req, env, actor)],
  ["GET", /^\/api\/admin\/presets\/([^/]+)$/, (req, env, actor, id) => api.presetDetail(env, id)],
  ["POST", /^\/api\/admin\/presets\/([^/]+)\/(approve|reject|delete|restore|purge|trust)$/,
    (req, env, actor, id, action) => api.presetAction(req, env, actor, id, action)],
  ["GET", /^\/api\/admin\/trusted$/, (req, env) => api.listTrusted(env)],
  ["POST", /^\/api\/admin\/trusted$/, (req, env, actor) => api.addTrusted(req, env, actor)],
  ["DELETE", /^\/api\/admin\/trusted\/([a-fA-F0-9]{64})$/, (req, env, actor, hash) => api.removeTrusted(env, actor, hash)],
  ["GET", /^\/api\/admin\/audit$/, (req, env) => api.listAudit(env, new URL(req.url).searchParams)],
  ["GET", /^\/api\/admin\/release$/, (req, env) => api.getRelease(env)],
  ["PUT", /^\/api\/admin\/release$/, (req, env, actor) => api.saveRelease(req, env, actor)],
  ["PUT", /^\/api\/admin\/release\/artifact$/, (req, env, actor) => api.uploadArtifact(req, env, actor)],
  ["POST", /^\/api\/admin\/catalog\/rebuild$/, (req, env, actor) => api.rebuild(env, actor)],
];

const SECURITY_HEADERS = {
  "content-security-policy": "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'",
  "x-content-type-options": "nosniff",
  "referrer-policy": "no-referrer",
  "cache-control": "no-store",
};

function withSecurityHeaders(response) {
  const headers = new Headers(response.headers);
  for (const [k, v] of Object.entries(SECURITY_HEADERS)) if (!headers.has(k)) headers.set(k, v);
  return new Response(response.body, { status: response.status, headers });
}

async function handle(request, env) {
  const actor = await authenticate(request, env);
  if (!actor) return text("Forbidden.", 403);

  const { pathname } = new URL(request.url);
  const method = request.method.toUpperCase();
  if (method === "GET" && ASSETS[pathname]) {
    return new Response(ASSETS[pathname].body, { headers: { "content-type": ASSETS[pathname].type } });
  }
  if (!isAllowedOrigin(request)) return bad("Cross-origin request refused.", 403);

  for (const [routeMethod, pattern, handler] of ROUTES) {
    if (method !== routeMethod) continue;
    const match = pathname.match(pattern);
    if (match) return handler(request, env, actor, ...match.slice(1));
  }
  return notFound();
}

export default {
  async fetch(request, env) {
    try {
      return withSecurityHeaders(await handle(request, env));
    } catch (error) {
      console.error(JSON.stringify({ event: "admin_request_failed", path: new URL(request.url).pathname, error: String(error?.stack || error) }));
      return withSecurityHeaders(bad("Server error.", 500));
    }
  },
};
