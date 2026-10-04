// Cloudflare Access authentication for the admin Worker.
//
// The admin Worker's whole hostname sits behind a Cloudflare Access
// application, so every request that reaches it carries a signed JWT in the
// `Cf-Access-Jwt-Assertion` header. We verify that token ourselves (signature
// against the team's published keys, audience, issuer, expiry) and then
// require the identity to be explicitly allowed. Nothing is trusted from
// plain headers, and missing configuration denies everything.
//
// Configuration (Worker vars):
//   ACCESS_TEAM_DOMAIN   e.g. "nostyy88.cloudflareaccess.com"
//   ACCESS_AUD           the Access application's audience tag
//   ADMIN_EMAILS         comma-separated emails allowed to administer
//   ADMIN_SERVICE_TOKENS comma-separated Access service-token client IDs (scripts), optional

const KEY_CACHE_MS = 60 * 60 * 1000;
let keyCache = { domain: "", fetchedAt: 0, keys: new Map() };

const list = (value) => String(value || "").split(",").map((s) => s.trim().toLowerCase()).filter(Boolean);

function base64UrlToBytes(value) {
  const b64 = value.replace(/-/g, "+").replace(/_/g, "/") + "===".slice((value.length + 3) % 4);
  return Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
}

const decodeJson = (part) => JSON.parse(new TextDecoder().decode(base64UrlToBytes(part)));

async function signingKey(teamDomain, kid) {
  const fresh = keyCache.domain === teamDomain && Date.now() - keyCache.fetchedAt < KEY_CACHE_MS;
  if (!fresh || !keyCache.keys.has(kid)) {
    const res = await fetch(`https://${teamDomain}/cdn-cgi/access/certs`);
    if (!res.ok) throw new Error(`Access certs request failed: ${res.status}`);
    const { keys = [] } = await res.json();
    const imported = new Map();
    for (const jwk of keys) {
      if (jwk.kty !== "RSA" || !jwk.kid) continue;
      imported.set(jwk.kid, await crypto.subtle.importKey("jwk", jwk, { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" }, false, ["verify"]));
    }
    keyCache = { domain: teamDomain, fetchedAt: Date.now(), keys: imported };
  }
  return keyCache.keys.get(kid) || null;
}

// Returns the verified JWT claims, or null.
async function verifyAccessJwt(token, env) {
  const teamDomain = String(env.ACCESS_TEAM_DOMAIN || "").trim();
  const audience = String(env.ACCESS_AUD || "").trim();
  if (!teamDomain || !audience || !token) return null;

  const parts = token.split(".");
  if (parts.length !== 3) return null;
  let header, claims;
  try {
    header = decodeJson(parts[0]);
    claims = decodeJson(parts[1]);
  } catch {
    return null;
  }
  if (header.alg !== "RS256" || !header.kid) return null;

  const key = await signingKey(teamDomain, header.kid);
  if (!key) return null;
  const signed = new TextEncoder().encode(`${parts[0]}.${parts[1]}`);
  if (!(await crypto.subtle.verify("RSASSA-PKCS1-v1_5", key, base64UrlToBytes(parts[2]), signed))) return null;

  const now = Math.floor(Date.now() / 1000);
  const audiences = Array.isArray(claims.aud) ? claims.aud : [claims.aud];
  if (!audiences.includes(audience)) return null;
  if (claims.iss !== `https://${teamDomain}`) return null;
  if (typeof claims.exp !== "number" || claims.exp <= now) return null;
  if (typeof claims.nbf === "number" && claims.nbf > now + 60) return null;
  return claims;
}

// Returns the acting identity ("admin:<email>" or "service:<client id>"), or null if not allowed.
export async function authenticate(request, env) {
  const claims = await verifyAccessJwt(request.headers.get("cf-access-jwt-assertion") || "", env);
  if (!claims) return null;
  const email = String(claims.email || "").toLowerCase();
  if (email) return list(env.ADMIN_EMAILS).includes(email) ? `admin:${email}` : null;
  // Service tokens (used by scripts) carry the token's client ID instead of an email.
  const serviceId = String(claims.common_name || "").toLowerCase();
  if (serviceId && list(env.ADMIN_SERVICE_TOKENS).includes(serviceId)) return `service:${serviceId}`;
  return null;
}
