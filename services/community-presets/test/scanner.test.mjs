import test from "node:test";
import assert from "node:assert/strict";
import { scanPresetIni } from "../src/index.js";

const validPreset = `[CrimsonWeatherPreset]
[Meta]
FormatVersion=6

[Weather]
ForceClearSky=0
NoRain=0
Rain=0.0000
Thunder=0.0000
NoDust=0
Dust=0.0000
NoSnow=0
Snow=0.0000
`;

test("accepts a canonical Crimson Weather preset", () => {
  const scan = scanPresetIni(validPreset);
  assert.equal(scan.ok, true);
  assert.equal(scan.formatVersion, 6);
});

test("accepts RenoDX aurora gate preset fields", () => {
  const scan = scanPresetIni(`${validPreset}
[RenoDX]
AuroraEnabled=1
AuroraRegionMask=126
`);
  assert.equal(scan.ok, true);
});

test("rejects missing header", () => {
  const scan = scanPresetIni(`[Weather]\nRain=1\n`);
  assert.equal(scan.ok, false);
  assert.match(scan.errors.join("\n"), /Missing/);
});

test("rejects unsafe paths and URLs", () => {
  const scan = scanPresetIni(`${validPreset}\n[Celestial]\nMoonTexture=https://bad.example/a.dds\n`);
  assert.equal(scan.ok, false);
  assert.match(scan.errors.join("\n"), /Unsafe/);
});

test("rejects unknown keys", () => {
  const scan = scanPresetIni(`${validPreset}\n[Weather]\nLaunchExe=calc.exe\n`);
  assert.equal(scan.ok, false);
  assert.match(scan.errors.join("\n"), /Unknown key/);
});
