// A stand-in for Cloudflare Access in tests: its own RSA signing key, served at
// the team's certs URL, and a helper to mint Access JWTs. The admin Worker's
// real verification code runs unchanged against it.
export const TEAM_DOMAIN = "test-team.cloudflareaccess.com";
export const AUDIENCE = "test-admin-aud";
export const ADMIN_EMAIL = "owner@example.com";
const KID = "test-key-1";

const keyPair = await crypto.subtle.generateKey(
  { name: "RSASSA-PKCS1-v1_5", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: "SHA-256" },
  true,
  ["sign", "verify"],
);
const publicJwk = { ...(await crypto.subtle.exportKey("jwk", keyPair.publicKey)), kid: KID, alg: "RS256", use: "sig" };

const b64url = (bytes) => Buffer.from(bytes).toString("base64url");

// Signs an Access-style JWT. `overrides` can break any part for negative tests.
export async function accessJwt(claims = {}, { key = keyPair.privateKey, kid = KID, alg = "RS256" } = {}) {
  const now = Math.floor(Date.now() / 1000);
  const header = b64url(JSON.stringify({ alg, kid, typ: "JWT" }));
  const body = b64url(JSON.stringify({ aud: [AUDIENCE], iss: `https://${TEAM_DOMAIN}`, email: ADMIN_EMAIL,
    iat: now, nbf: now, exp: now + 3600, ...claims }));
  const sig = await crypto.subtle.sign("RSASSA-PKCS1-v1_5", key, new TextEncoder().encode(`${header}.${body}`));
  return `${header}.${body}.${b64url(new Uint8Array(sig))}`;
}

// A second key pair, not published: tokens signed with it must be rejected.
export const foreignKey = (await crypto.subtle.generateKey(
  { name: "RSASSA-PKCS1-v1_5", modulusLength: 2048, publicExponent: new Uint8Array([1, 0, 1]), hash: "SHA-256" },
  true,
  ["sign"],
)).privateKey;

export const ACCESS_BINDINGS = {
  ACCESS_TEAM_DOMAIN: TEAM_DOMAIN,
  ACCESS_AUD: AUDIENCE,
  ADMIN_EMAILS: ADMIN_EMAIL,
  ADMIN_SERVICE_TOKENS: "release-script.access",
};

// Miniflare outbound handler: answers the Access certs request, refuses anything else.
export function accessOutbound(request) {
  const url = new URL(request.url);
  if (url.hostname === TEAM_DOMAIN && url.pathname === "/cdn-cgi/access/certs") {
    return new Response(JSON.stringify({ keys: [publicJwk] }), { headers: { "content-type": "application/json" } });
  }
  return new Response("unexpected outbound request", { status: 599 });
}
