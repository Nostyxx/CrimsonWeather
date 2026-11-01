import test from "node:test";
import assert from "node:assert/strict";
import { scanPresetIni } from "../src/index.js";

const waterPreset = `[CrimsonWeatherPreset]
[Meta]
FormatVersion=7

[Weather]
Rain=0.0000

[Water]
WaterMurkinessEnabled=1
WaterMurkiness=1
WaterRipplesEnabled=1
WaterRipples=1
WaterChopEnabled=1
WaterChop=1
WaterWavesEnabled=1
WaterWaves=1
WaterSwellEnabled=1
WaterSwell=1
UnderwaterTintStrengthEnabled=1
UnderwaterTintStrength=1
UnderwaterFogEnabled=1
UnderwaterFog=1
MurkColorEnabled=1
MurkColorR=1
MurkColorG=1
MurkColorB=1
UnderwaterTintColorEnabled=1
UnderwaterTintColorR=1
UnderwaterTintColorG=1
UnderwaterTintColorB=1

[Region.Hernand]
Enabled=1

[Region.Hernand.Water]
WaterSwellEnabled=1
WaterSwell=2.0000
`;

test("accepts a format 7 preset with Water sections and every water key", () => {
  const scan = scanPresetIni(waterPreset);
  assert.deepEqual(scan.errors, []);
  assert.equal(scan.ok, true);
  assert.equal(scan.formatVersion, 7);
});

test("still accepts format 6 presets holding the removed snow keys", () => {
  const scan = scanPresetIni(`[CrimsonWeatherPreset]\n[Meta]\nFormatVersion=6\n\n[Weather]\nSnowAccumBoundaryAEnabled=1\nSnowAccumBoundaryA=10\nSnowCoverageThreshold=5\n`);
  assert.deepEqual(scan.errors, []);
  assert.equal(scan.ok, true);
});

test("rejects unknown water keys and formats newer than 7", () => {
  const unknown = scanPresetIni(waterPreset.replace("WaterSwell=2.0000", "WaterSharpness=1"));
  assert.equal(unknown.ok, false);
  assert.match(unknown.errors.join("\n"), /Unknown key: WaterSharpness/);
  const newer = scanPresetIni(waterPreset.replace("FormatVersion=7", "FormatVersion=8"));
  assert.equal(newer.ok, false);
  assert.match(newer.errors.join("\n"), /newer than supported/);
});
