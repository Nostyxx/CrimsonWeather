// Contract test: every implementation must reproduce the golden transcripts
// recorded from the production Worker (see test/legacy/README.md).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { IMPLEMENTATIONS, runScenario } from "./run.mjs";
import { scenarios } from "./scenarios.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const golden = (name) => JSON.parse(readFileSync(join(here, "golden", `${name}.json`), "utf8"));

for (const [implName, impl] of Object.entries(IMPLEMENTATIONS)) {
  for (const name of Object.keys(scenarios)) {
    test(`${implName}: ${name}`, async () => {
      const expected = golden(name);
      const actual = await runScenario(impl, name);
      // Compare step by step so a failure names the first diverging step.
      for (let i = 0; i < Math.max(expected.length, actual.length); i++) {
        assert.deepStrictEqual(actual[i], expected[i], `step ${i}: "${expected[i]?.step ?? actual[i]?.step}"`);
      }
    });
  }
}
