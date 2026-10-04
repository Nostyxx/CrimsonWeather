ALTER TABLE presets ADD COLUMN update_of TEXT NOT NULL DEFAULT '';

CREATE INDEX IF NOT EXISTS idx_presets_submitter_updated ON presets(submitter_hash, updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_presets_update_of ON presets(update_of);

CREATE TABLE IF NOT EXISTS client_whitelist (
  submitter_hash TEXT PRIMARY KEY,
  label TEXT NOT NULL DEFAULT '',
  auto_approve INTEGER NOT NULL DEFAULT 1,
  note TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL
);
