export function createRouter(handlers) {
  const {
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
  } = handlers;

  return async function route(request, env) {
    const url = new URL(request.url);
    const method = request.method.toUpperCase();
    try {
      if (method === "GET" && url.pathname === "/api/v1/catalog") return await getCatalog(env);
      if (method === "GET" && url.pathname === "/api/v1/update") return await updateInfo(request, env);
      if (method === "GET" && url.pathname === "/api/v1/update/artifact") return await updateArtifact(request, env);
      if (method === "POST" && url.pathname === "/api/v1/presets") return await submitPreset(request, env);
      if (method === "GET" && url.pathname === "/api/v1/me/presets") return await listMyPresets(request, env);
      let match = url.pathname.match(/^\/api\/v1\/me\/presets\/([^/]+)\/update$/);
      if (method === "DELETE" && match) return await cancelMyPresetUpdate(request, env, match[1]);
      match = url.pathname.match(/^\/api\/v1\/me\/presets\/([^/]+)$/);
      if (method === "PUT" && match) return await updateMyPreset(request, env, match[1]);
      if (method === "DELETE" && match) return await deleteMyPreset(request, env, match[1]);
      match = url.pathname.match(/^\/api\/v1\/presets\/([^/]+)\/download$/);
      if (method === "GET" && match) return await downloadPreset(request, env, match[1]);
      match = url.pathname.match(/^\/api\/v1\/presets\/([^/]+)\/like$/);
      if (method === "POST" && match) return await toggleLike(request, env, match[1]);
      if (method === "POST" && url.pathname === "/api/v1/admin/login") return await loginAdmin(request, env);
      if (method === "POST" && url.pathname === "/api/v1/admin/logout") return logoutAdmin();
      if (url.pathname === "/admin" && method === "GET") {
        if (!(await isAdmin(request, env))) {
          if (!hasAdminLoginKey(request, env)) return text("Not found.", 404);
          return new Response(ADMIN_LOGIN_HTML, { headers: { "content-type": "text/html; charset=utf-8" } });
        }
        return new Response(ADMIN_HTML, { headers: { "content-type": "text/html; charset=utf-8" } });
      }
      if (url.pathname.startsWith("/api/v1/admin/")) {
        if (!(await isAdmin(request, env))) return bad("Unauthorized", 401);
        if (method === "GET" && url.pathname === "/api/v1/admin/overview") return await adminOverview(env);
        if (method === "GET" && url.pathname === "/api/v1/admin/update") return await adminGetUpdateSettings(env);
        if (method === "PUT" && url.pathname === "/api/v1/admin/update") return await adminSaveUpdateSettings(request, env);
        if (method === "PUT" && url.pathname === "/api/v1/admin/update/artifact") return await adminUploadUpdateArtifact(request, env);
        if (method === "GET" && url.pathname === "/api/v1/admin/presets") return await listAdminPresets(env, url.searchParams);
        if (method === "GET" && url.pathname === "/api/v1/admin/clients") return await listAdminClients(env, url.searchParams);
        if (method === "GET" && url.pathname === "/api/v1/admin/audit") return await listAdminAudit(env, url.searchParams);
        if (method === "GET" && url.pathname === "/api/v1/admin/submissions") return await listSubmissions(env, url.searchParams.get("status") || "pending");
        if (method === "GET" && url.pathname === "/api/v1/admin/submissions/export.zip") return await exportPending(env);
        if (method === "POST" && url.pathname === "/api/v1/admin/catalog/rebuild") return json({ ok: true, catalog: await rebuildCatalog(env) });
        if (method === "POST" && url.pathname === "/api/v1/admin/submissions/batch") return await batchSubmissions(request, env);
        if (method === "GET" && url.pathname === "/api/v1/admin/whitelist") return await listWhitelist(env);
        if (method === "POST" && url.pathname === "/api/v1/admin/whitelist") return await addWhitelist(request, env);
        match = url.pathname.match(/^\/api\/v1\/admin\/whitelist\/from-preset\/([^/]+)$/);
        if (method === "POST" && match) return await whitelistFromPreset(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/whitelist\/([a-fA-F0-9]{64})$/);
        if (method === "DELETE" && match) return await deleteWhitelist(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/presets\/([^/]+)\/restore$/);
        if (method === "POST" && match) return await restorePresetAdmin(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/presets\/([^/]+)\/purge$/);
        if (method === "POST" && match) return await purgePresetAdmin(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/presets\/([^/]+)$/);
        if (method === "GET" && match) return await adminPresetDetail(env, match[1]);
        if (method === "DELETE" && match) return await deletePresetAdmin(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/presets\/([^/]+)\/ini$/);
        if (method === "GET" && match) return await adminPresetIni(env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/submissions\/([^/]+)\/approve$/);
        if (method === "POST" && match) return await approveSubmission(request, env, match[1]);
        match = url.pathname.match(/^\/api\/v1\/admin\/submissions\/([^/]+)\/reject$/);
        if (method === "POST" && match) return await rejectSubmission(request, env, match[1]);
      }
      return bad("Not found.", 404);
    } catch (error) {
      console.error(JSON.stringify({ event: "community_request_failed", method, path: url.pathname, errorType: error?.name || "Error" }));
      return bad(error?.message || "Server error.", 500);
    }
  };
}
