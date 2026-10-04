

export function nowIso() {
  return new Date().toISOString();
}

export function addDaysIso(days) {
  return new Date(Date.now() + days * 24 * 60 * 60 * 1000).toISOString();
}
