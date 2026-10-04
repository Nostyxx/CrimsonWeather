const basePreset = {
  id: "hernand-clear-skies",
  title: "Hernand Clear Skies",
  author_name: "Example Author",
  description: "A clear weather preset.",
  tags_json: "[]",
  r2_key: "approved/hernand-clear-skies/preset.ini",
  sha256: "a".repeat(64),
  size_bytes: 128,
  format_version: 6,
  min_addon_version: "0.6.3",
  submitter_hash: "b".repeat(64),
  downloads: 3,
  likes: 1,
  created_at: "2026-01-01T00:00:00.000Z",
  updated_at: "2026-01-02T00:00:00.000Z",
  approved_at: "2026-01-02T00:00:00.000Z",
  rejected_at: null,
  update_of: "",
  deleted_at: null,
  delete_after: null
};

export const lifecycleFixtures = {
  approved: { ...basePreset, status: "approved" },
  pending: {
    ...basePreset,
    id: "hernand-evening-pending",
    status: "pending",
    r2_key: "pending/hernand-evening-pending/preset.ini",
    approved_at: null
  },
  rejected: {
    ...basePreset,
    id: "hernand-cloudy-rejected",
    status: "rejected",
    rejected_at: "2026-01-03T00:00:00.000Z"
  },
  deleted: {
    ...basePreset,
    status: "approved",
    deleted_at: "2026-01-03T00:00:00.000Z",
    delete_after: "2026-01-10T00:00:00.000Z",
    deleted_by: "admin",
    delete_reason: "fixture"
  },
  trustedOwner: {
    submitter_hash: "c".repeat(64),
    label: "Trusted creator",
    auto_approve: 1,
    note: "fixture",
    created_at: "2026-01-01T00:00:00.000Z",
    updated_at: "2026-01-01T00:00:00.000Z"
  },
  pendingUpdate: {
    ...basePreset,
    id: "hernand-clear-skies-update-00000001",
    status: "pending",
    update_of: "hernand-clear-skies",
    r2_key: "pending/hernand-clear-skies-update-00000001/preset.ini",
    approved_at: null
  }
};

export const catalogAuthorFixture = {
  catalog: { ...basePreset, status: undefined, author: "Example Author" },
  ownerList: { ...basePreset, author_name: "Example Author" }
};
