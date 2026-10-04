ALTER TABLE presets ADD COLUMN deleted_at TEXT;
ALTER TABLE presets ADD COLUMN delete_after TEXT;
ALTER TABLE presets ADD COLUMN deleted_by TEXT NOT NULL DEFAULT '';
ALTER TABLE presets ADD COLUMN delete_reason TEXT NOT NULL DEFAULT '';

CREATE INDEX IF NOT EXISTS idx_presets_deleted_after ON presets(deleted_at, delete_after);
CREATE INDEX IF NOT EXISTS idx_presets_public_catalog ON presets(status, update_of, deleted_at, updated_at DESC);
CREATE INDEX IF NOT EXISTS idx_presets_client_deleted ON presets(submitter_hash, deleted_at, updated_at DESC);
