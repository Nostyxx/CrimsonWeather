

export async function whitelistRow(env, submitterHash) {
  if (!submitterHash) return null;
  return env.DB.prepare("SELECT * FROM client_whitelist WHERE submitter_hash=? AND auto_approve=1").bind(submitterHash).first();
}
