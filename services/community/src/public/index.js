// Public community Worker: the /api/v1 API the addon talks to.
//
// This Worker has no admin functionality; moderation lives in the separate,
// Access-protected admin Worker (src/admin). The routes and responses here are
// a frozen contract with every shipped addon version: see test/golden.
import { bad, notFound } from "../shared/http.js";
import { serveCatalog } from "../shared/catalog.js";
import { purgeExpiredPresets } from "../shared/presets.js";
import { updateArtifact, updateInfo } from "./updates.js";
import {
  cancelMyPresetUpdate,
  deleteMyPreset,
  downloadPreset,
  listMyPresets,
  submitPreset,
  toggleLike,
  updateMyPreset,
} from "./presets.js";

// [method, pattern, handler(request, env, ...params)]
const ROUTES = [
  ["GET", /^\/api\/v1\/catalog$/, (req, env) => serveCatalog(env)],
  ["GET", /^\/api\/v1\/update$/, updateInfo],
  ["GET", /^\/api\/v1\/update\/artifact$/, updateArtifact],
  ["POST", /^\/api\/v1\/presets$/, submitPreset],
  ["GET", /^\/api\/v1\/presets\/([^/]+)\/download$/, downloadPreset],
  ["POST", /^\/api\/v1\/presets\/([^/]+)\/like$/, toggleLike],
  ["GET", /^\/api\/v1\/me\/presets$/, listMyPresets],
  ["PUT", /^\/api\/v1\/me\/presets\/([^/]+)$/, updateMyPreset],
  ["DELETE", /^\/api\/v1\/me\/presets\/([^/]+)$/, deleteMyPreset],
  ["DELETE", /^\/api\/v1\/me\/presets\/([^/]+)\/update$/, cancelMyPresetUpdate],
];

export default {
  async fetch(request, env) {
    const { pathname } = new URL(request.url);
    const method = request.method.toUpperCase();
    try {
      for (const [routeMethod, pattern, handler] of ROUTES) {
        if (method !== routeMethod) continue;
        const match = pathname.match(pattern);
        if (match) return await handler(request, env, ...match.slice(1));
      }
      return notFound();
    } catch (error) {
      // Details go to the Worker log, never to clients.
      console.error(JSON.stringify({ event: "request_failed", method, path: pathname, error: String(error?.stack || error) }));
      return bad("Server error.", 500);
    }
  },

  async scheduled(_event, env) {
    await purgeExpiredPresets(env);
  },
};
