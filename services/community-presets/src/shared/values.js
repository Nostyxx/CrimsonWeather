

export function sanitizeText(value, maxLen) {
  return String(value ?? "").replace(/[\u0000-\u001f\u007f]/g, " ").replace(/\s+/g, " ").trim().slice(0, maxLen);
}

export function slugify(value) {
  return sanitizeText(value, 80).toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 48) || "preset";
}

export function bytesOf(textValue) {
  return new TextEncoder().encode(textValue);
}

export function normalizeLimit(value, fallback = 100, max = 500) {
  const parsed = Number.parseInt(value || "", 10);
  if (!Number.isFinite(parsed) || parsed <= 0) return fallback;
  return Math.min(parsed, max);
}

export function likeTerm(value) {
  const clean = sanitizeText(value, 120);
  return clean ? `%${clean}%` : "";
}
