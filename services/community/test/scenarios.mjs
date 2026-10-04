// Client-visible behaviour of the public /api/v1 routes, as scenarios.
// Each scenario gets a fresh Worker and records a transcript through `t`
// (a Recorder). Admin steps go through `t.admin` (see adapters.mjs).

const ALICE = "client-alice-0001";
const BOB = "client-bob-0002";
const CAROL = "client-carol-0003";

export const PRESET = [
  "[CrimsonWeatherPreset]",
  "FormatVersion=6",
  "; comment lines are ignored",
  "[Weather]",
  "Rain=0.5",
  "NoSnow=1",
  "Thunder=",
  "[Celestial]",
  "MoonTextureEnabled=true",
  "MoonTexture=moon_red.dds",
  "[Region.Hernand]",
  "Rain=1",
  "[RenoDX]",
  "AuroraRegionMask=7",
].join("\r\n");

const PRESET_V2 = PRESET.replace("Rain=0.5", "Rain=0.75");
const PRESET_V3 = PRESET.replace("Rain=0.5", "Rain=0.9");

const submit = (t, label, client, fields = {}) =>
  t.call(label, "POST", "/api/v1/presets", {
    client,
    json: { title: "Clean Morning", authorName: "Alice", description: "Soft light", iniText: PRESET, clientVersion: "0.8.1", ...fields },
  });

export const scenarios = {
  async catalog_empty(t) {
    await t.call("first catalog read builds the catalog", "GET", "/api/v1/catalog");
    await t.call("second read is served from R2", "GET", "/api/v1/catalog");
    await t.snapshot("state");
  },

  async submit_validation(t) {
    await t.call("no client id", "POST", "/api/v1/presets", { json: { title: "x", iniText: PRESET } });
    await t.call("client id too long", "POST", "/api/v1/presets", { client: "x".repeat(129), json: { title: "x", iniText: PRESET } });
    await t.call("invalid json", "POST", "/api/v1/presets", { client: ALICE, body: "{not json", headers: { "content-type": "application/json" } });
    await t.call("missing title", "POST", "/api/v1/presets", { client: ALICE, json: { title: "   ", iniText: PRESET } });
    await t.call("empty preset", "POST", "/api/v1/presets", { client: ALICE, json: { title: "t", iniText: "" } });
    await t.call("missing header", "POST", "/api/v1/presets", { client: ALICE, json: { title: "t", iniText: "[Weather]\nRain=1" } });
    await t.call("unknown section and key", "POST", "/api/v1/presets", { client: ALICE,
      json: { title: "t", iniText: "[CrimsonWeatherPreset]\n[Evil]\nFoo=1\nbare line" } });
    await t.call("unsafe values", "POST", "/api/v1/presets", { client: ALICE,
      json: { title: "t", iniText: "[CrimsonWeatherPreset]\n[Celestial]\nMoonTexture=https://evil/x.dds\nMilkywayTexture=..\\..\\secret\nRain=C:\\x\n" } });
    await t.call("non-numeric value", "POST", "/api/v1/presets", { client: ALICE,
      json: { title: "t", iniText: "[CrimsonWeatherPreset]\n[Weather]\nRain=lots\nSnow=Infinity\nNoRain=yes" } });
    await t.call("future format version", "POST", "/api/v1/presets", { client: ALICE,
      json: { title: "t", iniText: "[CrimsonWeatherPreset]\nFormatVersion=99" } });
    await t.call("control characters", "POST", "/api/v1/presets", { client: ALICE,
      json: { title: "t", iniText: "[CrimsonWeatherPreset]\n[Weather]\nRain=1\u0007" } });
    await t.call("too large", "POST", "/api/v1/presets", { client: ALICE,
      json: { title: "t", iniText: "[CrimsonWeatherPreset]\n" + "; pad\n".repeat(12000) } });
    await t.call("titles and text are sanitized", "POST", "/api/v1/presets", { client: ALICE,
      json: { title: "  Storm\tNight  " + "x".repeat(100), authorName: "", description: "line1\nline2", iniText: PRESET } });
    await t.call("my presets", "GET", "/api/v1/me/presets", { client: ALICE });
    await t.call("my presets without client id", "GET", "/api/v1/me/presets");
    await t.call("pending preset is not public", "GET", "/api/v1/catalog");
    await t.snapshot("state");
  },

  async approve_download_like(t) {
    const { id } = await submit(t, "submit", ALICE);
    await t.call("download while pending", "GET", `/api/v1/presets/${id}/download`, { client: BOB });
    await t.call("like while pending", "POST", `/api/v1/presets/${id}/like`, { client: BOB });
    await t.admin.approve(id);
    await t.call("catalog after approval", "GET", "/api/v1/catalog");
    await t.call("download by bob", "GET", `/api/v1/presets/${id}/download`, { client: BOB });
    await t.call("download by bob again same day", "GET", `/api/v1/presets/${id}/download`, { client: BOB });
    await t.call("anonymous download", "GET", `/api/v1/presets/${id}/download`);
    await t.call("download by carol", "GET", `/api/v1/presets/${id}/download`, { client: CAROL });
    await t.call("like without client id", "POST", `/api/v1/presets/${id}/like`);
    await t.call("bob likes", "POST", `/api/v1/presets/${id}/like`, { client: BOB });
    await t.call("carol likes", "POST", `/api/v1/presets/${id}/like`, { client: CAROL });
    await t.call("bob unlikes", "POST", `/api/v1/presets/${id}/like`, { client: BOB });
    await t.call("catalog counts", "GET", "/api/v1/catalog");
    await t.call("unknown preset download", "GET", "/api/v1/presets/nope-12345678/download");
    await t.call("unknown preset like", "POST", "/api/v1/presets/nope-12345678/like", { client: BOB });
    await t.call("owner list", "GET", "/api/v1/me/presets", { client: ALICE });
    await t.snapshot("state");
  },

  async reject_and_catalog_order(t) {
    const a = await submit(t, "submit a", ALICE, { title: "Alpha" });
    const b = await submit(t, "submit b", BOB, { title: "Beta", authorName: "Bob" });
    const c = await submit(t, "submit c", CAROL, { title: "Gamma", authorName: "Carol" });
    await t.admin.approve(a.id);
    await t.admin.approve(b.id);
    await t.admin.reject(c.id, "not good");
    await t.call("catalog lists approved newest first", "GET", "/api/v1/catalog");
    await t.call("rejected owner sees status", "GET", "/api/v1/me/presets", { client: CAROL });
    await t.snapshot("state");
  },

  async trusted_submitter(t) {
    const first = await submit(t, "first submission", ALICE);
    await t.admin.approve(first.id);
    await t.admin.trustSubmitterOf(first.id);
    await submit(t, "trusted submission is auto-approved", ALICE, { title: "Second One" });
    await t.call("catalog includes it immediately", "GET", "/api/v1/catalog");
    await t.call("trusted owner edit applies directly", "PUT", `/api/v1/me/presets/${first.id}`, {
      client: ALICE, json: { title: "Clean Morning v2", authorName: "Alice", description: "edited", iniText: PRESET_V2, clientVersion: "0.8.1" } });
    await t.call("catalog shows the edit", "GET", "/api/v1/catalog");
    await t.snapshot("state");
  },

  async owner_updates(t) {
    const { id } = await submit(t, "submit", ALICE);
    await t.call("edit while pending changes it in place", "PUT", `/api/v1/me/presets/${id}`, {
      client: ALICE, json: { title: "Clean Morning", authorName: "Alice", description: "fixed typo", iniText: PRESET, clientVersion: "0.8.1" } });
    await t.admin.approve(id);
    await t.call("update approved preset -> pending update", "PUT", `/api/v1/me/presets/${id}`, {
      client: ALICE, json: { title: "Clean Morning", authorName: "Alice", description: "v2", iniText: PRESET_V2, clientVersion: "0.8.1" } });
    await t.call("second update supersedes the first", "PUT", `/api/v1/me/presets/${id}`, {
      client: ALICE, json: { title: "Clean Morning", authorName: "Alice", description: "v3", iniText: PRESET_V3, clientVersion: "0.8.1" } });
    await t.call("owner sees pending update", "GET", "/api/v1/me/presets", { client: ALICE });
    await t.call("catalog still shows approved version", "GET", "/api/v1/catalog");
    await t.call("other client cannot update", "PUT", `/api/v1/me/presets/${id}`, {
      client: BOB, json: { title: "Hijack", iniText: PRESET } });
    await t.call("update without title", "PUT", `/api/v1/me/presets/${id}`, { client: ALICE, json: { title: "", iniText: PRESET } });
    await t.call("update with invalid preset", "PUT", `/api/v1/me/presets/${id}`, { client: ALICE, json: { title: "x", iniText: "nope" } });
    await t.call("cancel pending update", "DELETE", `/api/v1/me/presets/${id}/update`, { client: ALICE });
    await t.call("cancel again finds nothing", "DELETE", `/api/v1/me/presets/${id}/update`, { client: ALICE });
    await t.call("cancel on someone else's preset", "DELETE", `/api/v1/me/presets/${id}/update`, { client: BOB });
    const upd = await t.call("new update after cancel", "PUT", `/api/v1/me/presets/${id}`, {
      client: ALICE, json: { title: "Clean Morning Final", authorName: "Alice", description: "v4", iniText: PRESET_V3, clientVersion: "0.8.1" } });
    await t.admin.approve(upd.updateId);
    await t.call("catalog shows approved update", "GET", "/api/v1/catalog");
    await t.call("download serves new content", "GET", `/api/v1/presets/${id}/download`, { client: BOB });
    await t.call("owner list after update", "GET", "/api/v1/me/presets", { client: ALICE });
    await t.snapshot("state");
  },

  async owner_delete_and_purge(t) {
    const a = await submit(t, "submit a", ALICE, { title: "Keep Me" });
    const b = await submit(t, "submit b", ALICE, { title: "Delete Me" });
    await t.admin.approve(a.id);
    await t.admin.approve(b.id);
    await t.call("like before delete", "POST", `/api/v1/presets/${b.id}/like`, { client: BOB });
    await t.call("other client cannot delete", "DELETE", `/api/v1/me/presets/${b.id}`, { client: BOB });
    await t.call("owner deletes", "DELETE", `/api/v1/me/presets/${b.id}`, { client: ALICE });
    await t.call("delete twice", "DELETE", `/api/v1/me/presets/${b.id}`, { client: ALICE });
    await t.call("catalog without deleted", "GET", "/api/v1/catalog");
    await t.call("deleted preset download", "GET", `/api/v1/presets/${b.id}/download`, { client: BOB });
    await t.call("owner list hides deleted", "GET", "/api/v1/me/presets", { client: ALICE });
    await t.snapshot("after delete (retained for 7 days)");
    await t.worker.scheduled();
    await t.snapshot("scheduled run before expiry keeps it");
    await t.worker.db.prepare("UPDATE presets SET delete_after='2000-01-01T00:00:00.000Z' WHERE deleted_at IS NOT NULL").run();
    await t.worker.scheduled();
    await t.snapshot("scheduled run after expiry purges it");
    await t.call("catalog after purge", "GET", "/api/v1/catalog");
  },

  async admin_moderation_effects(t) {
    const a = await submit(t, "submit a", ALICE, { title: "Moderated" });
    await t.admin.approve(a.id);
    await t.admin.softDelete(a.id);
    await t.call("catalog after admin delete", "GET", "/api/v1/catalog");
    await t.call("owner list after admin delete", "GET", "/api/v1/me/presets", { client: ALICE });
    await t.admin.restore(a.id);
    await t.call("catalog after restore", "GET", "/api/v1/catalog");
    await t.call("like after restore", "POST", `/api/v1/presets/${a.id}/like`, { client: BOB });
    await t.admin.purge(a.id);
    await t.call("catalog after purge", "GET", "/api/v1/catalog");
    await t.snapshot("state");
  },

  async update_check(t) {
    await t.call("default update info", "GET", "/api/v1/update");
    await t.call("default update info with version", "GET", "/api/v1/update?version=0.6.8");
    await t.call("artifact not configured", "GET", "/api/v1/update/artifact");
    await t.admin.publishUpdate({ latestVersion: "0.8.1", downloadPageUrl: "https://www.nexusmods.com/crimsondesert/mods/632?tab=files",
      changelog: "Update 0.8.1\r\n- Updated for 2.03.00", publishedAt: "", critical: false });
    await t.call("published notice, no artifact yet", "GET", "/api/v1/update", { clientVersion: "0.8.0" });
    await t.admin.uploadArtifact("0.8.1", new TextEncoder().encode("MZ fake addon 0.8.1"));
    await t.call("older client sees update", "GET", "/api/v1/update", { clientVersion: "0.8.0" });
    await t.call("same version: no update", "GET", "/api/v1/update?version=0.8.1");
    await t.call("newer client: no update", "GET", "/api/v1/update?version=v0.9.0 beta");
    await t.call("channel header echoed", "GET", "/api/v1/update", { headers: { "x-cw-channel": "beta" } });
    await t.call("download artifact", "GET", "/api/v1/update/artifact?version=0.8.1");
    await t.call("download without version", "GET", "/api/v1/update/artifact");
    await t.call("wrong version", "GET", "/api/v1/update/artifact?version=0.7.0");
    await t.admin.publishUpdate({ latestVersion: "0.8.2", downloadPageUrl: "https://example.com/files",
      changelog: "Update 0.8.2", publishedAt: "2026-10-01", critical: true });
    await t.call("new version clears the artifact", "GET", "/api/v1/update?version=0.8.1");
    await t.call("artifact gone after version change", "GET", "/api/v1/update/artifact?version=0.8.2");
    await t.snapshot("state");
  },

  async routing(t) {
    await t.call("unknown route", "GET", "/api/v1/nope");
    await t.call("wrong method on catalog", "POST", "/api/v1/catalog");
    await t.call("root", "GET", "/");
  },
};
