// Records test/golden/<scenario>.json from the legacy (production) Worker.
// Run: npm run golden [scenario...]
import { isDeepStrictEqual } from "node:util";
import { mkdirSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { runScenario, IMPLEMENTATIONS } from "./run.mjs";
import { scenarios } from "./scenarios.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const names = process.argv.slice(2).length ? process.argv.slice(2) : Object.keys(scenarios);
mkdirSync(join(here, "golden"), { recursive: true });

for (const name of names) {
  // Run twice: a golden file is only trustworthy if the oracle is deterministic.
  const first = await runScenario(IMPLEMENTATIONS.legacy, name);
  const second = await runScenario(IMPLEMENTATIONS.legacy, name);
  if (!isDeepStrictEqual(first, second)) {
    console.error(`${name}: legacy transcript is not deterministic, not recording`);
    process.exitCode = 1;
    continue;
  }
  writeFileSync(join(here, "golden", `${name}.json`), JSON.stringify(first, null, 2) + "\n");
  console.log(`${name}: ${first.length} steps`);
}
