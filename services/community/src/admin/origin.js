// State-changing requests must come from our own page (browsers send Origin) or
// from a script (no Origin). Blocks other sites from driving an admin's session.
export function isAllowedOrigin(request) {
  if (request.method.toUpperCase() === "GET") return true;
  const origin = request.headers.get("origin");
  return !origin || origin === new URL(request.url).origin;
}
