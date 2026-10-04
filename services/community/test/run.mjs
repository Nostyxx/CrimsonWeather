// Runs one scenario against one implementation and returns its transcript.
import { existsSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { startWorker, Recorder, TEST_ENV } from "./harness.mjs";
import { legacyAdmin, nextAdmin } from "./adapters.mjs";
import { ACCESS_BINDINGS, accessOutbound } from "./access-fixture.mjs";
import { scenarios } from "./scenarios.mjs";
import { buildWorkers } from "../build.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const dist = join(here, "..", "dist");

export const IMPLEMENTATIONS = {
  // The production Worker as deployed (behavior reference).
  legacy: { scriptPath: join(here, "legacy", "worker.js"), allMigrations: false, admin: legacyAdmin },
  // The rewrite: public Worker + Access-protected admin Worker, built from src/.
  next: {
    scriptPath: join(dist, "public", "index.js"),
    adminScriptPath: join(dist, "admin", "index.js"),
    allMigrations: true,
    adminBindings: { ...ACCESS_BINDINGS, DEVICE_HASH_SECRET: TEST_ENV.DEVICE_HASH_SECRET },
    adminOutbound: accessOutbound,
    admin: nextAdmin,
    build: true,
  },
};

let built = null;
export async function prepare(impl) {
  if (impl.build) built ??= buildWorkers();
  await built;
}

export async function runScenario(impl, name) {
  await prepare(impl);
  if (!existsSync(impl.scriptPath)) throw new Error(`missing worker script ${impl.scriptPath}`);
  const worker = await startWorker(impl);
  try {
    const t = new Recorder(worker, impl.admin(worker));
    await scenarios[name](t);
    return t.transcript();
  } finally {
    await worker.dispose();
  }
}
