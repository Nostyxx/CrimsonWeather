ALTER TABLE presets ADD COLUMN content_revision INTEGER NOT NULL DEFAULT 0;

CREATE TABLE IF NOT EXISTS preset_object_staging (
  r2_key TEXT PRIMARY KEY,
  created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS preset_object_cleanup (
  r2_key TEXT PRIMARY KEY,
  created_at TEXT NOT NULL
);
