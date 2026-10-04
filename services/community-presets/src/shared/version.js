

export function normalizeVersion(value) {
  return String(value || "").trim().replace(/^v/i, "").replace(/\s+.*$/, "");
}

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
