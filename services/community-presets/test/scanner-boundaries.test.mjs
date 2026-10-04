import test from "node:test";
import assert from "node:assert/strict";
import { scanPresetIni } from "../src/index.js";

const validPreset = `[CrimsonWeatherPreset]
[Meta]
FormatVersion=6

[Weather]
Rain=0.2500
`;

test("scanner byte limit uses UTF-8 byte count and accepts the exact limit", () => {
  const bytes = new TextEncoder().encode(validPreset).byteLength;
  assert.equal(scanPresetIni(validPreset, bytes).ok, true);
  const overLimit = scanPresetIni(validPreset, bytes - 1);
  assert.equal(overLimit.ok, false);
  assert.match(overLimit.errors.join("\n"), /exceeds/);
});

test("scanner rejects a format newer than the current addon", () => {
  const scan = scanPresetIni(validPreset.replace("FormatVersion=6", "FormatVersion=7"));
  assert.equal(scan.ok, false);
  assert.match(scan.errors.join("\n"), /newer than supported/);
});

test("scanner accepts inherited region sections and preserves duplicate-key behavior", () => {
  const scan = scanPresetIni(`${validPreset}\n[Region.Hernand]\nRain=0.5\nRain=0.75\n`);
  assert.deepEqual(scan.errors, []);
  assert.equal(scan.ok, true);
});

test("scanner rejects unknown sections, malformed numbers, and unsafe texture paths", () => {
  for (const content of [
    `${validPreset}\n[UnknownSection]\nRain=0.5\n`,
    `${validPreset}\n[Weather]\nRain=not-a-number\n`,
    `${validPreset}\n[Celestial]\nMoonTexture=../outside.dds\n`
  ]) {
    assert.equal(scanPresetIni(content).ok, false, content);
  }
});
