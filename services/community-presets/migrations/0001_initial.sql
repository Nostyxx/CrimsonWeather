CREATE TABLE IF NOT EXISTS presets (
  id TEXT PRIMARY KEY,
  title TEXT NOT NULL,
  author_name TEXT NOT NULL,
  description TEXT NOT NULL DEFAULT '',
  tags_json TEXT NOT NULL DEFAULT '[]',
  status TEXT NOT NULL CHECK (status IN ('pending', 'approved', 'rejected')),
  r2_key TEXT NOT NULL,
  sha256 TEXT NOT NULL,
  size_bytes INTEGER NOT NULL,
  format_version INTEGER NOT NULL DEFAULT 0,
  min_addon_version TEXT NOT NULL DEFAULT '0.6.3',
  submitter_hash TEXT NOT NULL DEFAULT '',
  safety_status TEXT NOT NULL DEFAULT 'pending',
  safety_summary TEXT NOT NULL DEFAULT '',
  downloads INTEGER NOT NULL DEFAULT 0,
  likes INTEGER NOT NULL DEFAULT 0,
  created_at TEXT NOT NULL,
  updated_at TEXT NOT NULL,
  approved_at TEXT,
  rejected_at TEXT
);

CREATE INDEX IF NOT EXISTS idx_presets_status_updated ON presets(status, updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_presets_status_downloads ON presets(status, downloads DESC);
CREATE INDEX IF NOT EXISTS idx_presets_status_likes ON presets(status, likes DESC);

CREATE TABLE IF NOT EXISTS preset_likes (
  preset_id TEXT NOT NULL REFERENCES presets(id) ON DELETE CASCADE,
  device_hash TEXT NOT NULL,
  created_at TEXT NOT NULL,
  PRIMARY KEY (preset_id, device_hash)
);

CREATE TABLE IF NOT EXISTS preset_downloads_daily (
  preset_id TEXT NOT NULL REFERENCES presets(id) ON DELETE CASCADE,
  device_hash TEXT NOT NULL,
  day TEXT NOT NULL,
  created_at TEXT NOT NULL,
  PRIMARY KEY (preset_id, device_hash, day)
);

CREATE TABLE IF NOT EXISTS admin_audit (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  action TEXT NOT NULL,
  preset_id TEXT,
  admin_email_or_token TEXT NOT NULL DEFAULT '',
  note TEXT NOT NULL DEFAULT '',
  created_at TEXT NOT NULL
);
