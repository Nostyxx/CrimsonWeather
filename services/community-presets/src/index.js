import { scanPresetIni } from "./presets/scanner.js";
import { createRouter } from "./http/router.js";
import { bad, json, text } from "./http/responses.js";
import { getCatalog, rebuildCatalog } from "./presets/catalog.js";
import { adminGetUpdateSettings, adminSaveUpdateSettings, updateInfo } from "./updates/settings.js";
import { adminUploadUpdateArtifact, updateArtifact } from "./updates/artifacts.js";
import { cancelMyPresetUpdate, deleteMyPreset, listMyPresets, submitPreset, updateMyPreset } from "./presets/submissions.js";
import { downloadPreset, toggleLike } from "./presets/interactions.js";
import { hasAdminLoginKey, isAdmin, loginAdmin, logoutAdmin } from "./auth/admin-session.js";
import { ADMIN_HTML, ADMIN_LOGIN_HTML } from "./admin/pages.js";
import { adminOverview, adminPresetDetail, adminPresetIni, listAdminAudit, listAdminClients, listAdminPresets, listSubmissions } from "./admin/queries.js";
import { exportPending } from "./admin/export.js";
import { batchSubmissions } from "./admin/batch.js";
import { addWhitelist, deleteWhitelist, listWhitelist, whitelistFromPreset } from "./admin/whitelist.js";
import { approveSubmission, deletePresetAdmin, purgePresetAdmin, rejectSubmission, restorePresetAdmin } from "./presets/moderation.js";
import { purgeExpiredDeletedPresets } from "./jobs/purge-deleted.js";

export { scanPresetIni };

const route = createRouter({
  json,
  text,
  bad,
  getCatalog,
  updateInfo,
  updateArtifact,
  submitPreset,
  listMyPresets,
  cancelMyPresetUpdate,
  updateMyPreset,
  deleteMyPreset,
  downloadPreset,
  toggleLike,
  loginAdmin,
  logoutAdmin,
  isAdmin,
  hasAdminLoginKey,
  ADMIN_LOGIN_HTML,
  ADMIN_HTML,
  adminOverview,
  adminGetUpdateSettings,
  adminSaveUpdateSettings,
  adminUploadUpdateArtifact,
  listAdminPresets,
  listAdminClients,
  listAdminAudit,
  listSubmissions,
  exportPending,
  rebuildCatalog,
  batchSubmissions,
  listWhitelist,
  addWhitelist,
  whitelistFromPreset,
  deleteWhitelist,
  restorePresetAdmin,
  purgePresetAdmin,
  adminPresetDetail,
  deletePresetAdmin,
  adminPresetIni,
  approveSubmission,
  rejectSubmission
});

export default {
  fetch: route,
  async scheduled(_event, env, _ctx) {
    try {
      await purgeExpiredDeletedPresets(env);
    } catch (error) {
      console.error(JSON.stringify({ event: "community_scheduled_failed", errorType: error?.name || "Error" }));
      throw error;
    }
  }
};
