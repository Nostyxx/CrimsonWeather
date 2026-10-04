// Small value helpers (text, versions, time).

export const nowIso = () => new Date().toISOString();

export const addDaysIso = (days) => new Date(Date.now() + days * 24 * 60 * 60 * 1000).toISOString();

// Single-line, control-free, trimmed and length-capped text.
export function sanitizeText(value, maxLen) {
  return String(value ?? "")
    .replace(/[\u0000-\u001f\u007f]/g, " ")
    .replace(/\s+/g, " ")
    .trim()
    .slice(0, maxLen);
}

export function slugify(value) {
  return sanitizeText(value, 80).toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "").slice(0, 48) || "preset";
}

// "v0.8.1 beta" -> "0.8.1"
export function normalizeVersion(value) {
  return String(value || "").trim().replace(/^v/i, "").replace(/\s+.*$/, "");
}

export const isVersion = (value) => /^\d+(?:\.\d+){1,3}$/.test(value);

export function compareVersions(a, b) {
  const aa = normalizeVersion(a).split(".").map((part) => Number.parseInt(part, 10) || 0);
  const bb = normalizeVersion(b).split(".").map((part) => Number.parseInt(part, 10) || 0);
  const count = Math.max(aa.length, bb.length, 3);
  for (let i = 0; i < count; i++) {
    const av = aa[i] || 0;
    const bv = bb[i] || 0;
    if (av !== bv) return av > bv ? 1 : -1;
  }
  return 0;
}
