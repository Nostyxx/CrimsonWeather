export async function getApprovedPresetRows(env) {
  const { results } = await env.DB.prepare(
    "SELECT id,title,author_name,description,tags_json,sha256,size_bytes,format_version,min_addon_version,downloads,likes,created_at,updated_at,approved_at FROM presets WHERE status='approved' AND update_of='' AND deleted_at IS NULL ORDER BY updated_at DESC"
  ).all();
  return results || [];
}

export async function nextCatalogGeneration(env, now) {
  const row = await env.DB.prepare(
    `INSERT INTO app_settings (key,value,updated_at,updated_by)
     VALUES ('catalog.generation','1',?,'catalog')
     ON CONFLICT(key) DO UPDATE SET
       value=CAST(app_settings.value AS INTEGER)+1,
       updated_at=excluded.updated_at,
       updated_by='catalog'
     RETURNING CAST(value AS INTEGER) AS generation`
  ).bind(now).first();
  const generation = Number(row?.generation);
  if (!Number.isSafeInteger(generation) || generation < 1) {
    throw new Error("Could not allocate a catalog generation.");
  }
  return generation;
}

export async function purgePresetRecord(env, row, identity, action, note, at, expiredBefore = null) {
  const guard = `id=? AND r2_key=? AND content_revision=?${expiredBefore ? " AND deleted_at IS NOT NULL AND delete_after<=?" : ""}`;
  const guardValues = [row.id, row.r2_key, row.content_revision, ...(expiredBefore ? [expiredBefore] : [])];
  const statements = [env.DB.prepare(
    `INSERT INTO admin_audit (action,preset_id,admin_email_or_token,note,created_at)
     SELECT ?,?,?,?,? FROM presets WHERE ${guard}`
  ).bind(action, row.id, identity, note, at, ...guardValues)];
  // Include the legacy fixed keys. Current revisions are immutable, and older
  // revisions were queued when they stopped being referenced.
  for (const key of new Set([row.r2_key, `pending/${row.id}/preset.ini`, `approved/${row.id}/preset.ini`])) {
    statements.push(env.DB.prepare(
      `INSERT INTO preset_object_cleanup (r2_key,created_at) SELECT ?,? FROM presets WHERE ${guard} ON CONFLICT(r2_key) DO NOTHING`
    ).bind(key, at, ...guardValues));
  }
  statements.push(env.DB.prepare(
    `DELETE FROM preset_likes WHERE preset_id=? AND EXISTS (SELECT 1 FROM presets WHERE ${guard})`
  ).bind(row.id, ...guardValues));
  statements.push(env.DB.prepare(
    `DELETE FROM preset_downloads_daily WHERE preset_id=? AND EXISTS (SELECT 1 FROM presets WHERE ${guard})`
  ).bind(row.id, ...guardValues));
  const deleteIndex = statements.length;
  statements.push(env.DB.prepare(`DELETE FROM presets WHERE ${guard}`).bind(...guardValues));
  const results = await env.DB.batch(statements);
  return Number(results[deleteIndex]?.meta?.changes || 0) > 0;
}
