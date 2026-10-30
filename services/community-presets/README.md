# Community presets Worker

The service remains one Cloudflare Worker with D1 (`DB`) and R2 (`PRESETS`). Its public `/api/v1` routes and admin pages remain compatible with the existing addon.

## Development checks

Use Node.js 22 or newer (the current verification used Node.js 24), then run:

```sh
npm ci
npm test
```

The integration suite bundles the Worker and starts a local Miniflare runtime with isolated D1/R2 storage. It applies the checked-in migrations and exercises submission, approval, likes, downloads, owner updates, cancellation, deletion, restoration and purge. It also exercises concurrent likes/downloads. It does not use Cloudflare credentials or remote resources. Pure tests cover scanner boundaries and catalog publication conflicts/failures.

## Source ownership

- `index.js`: composition and scheduled entry point.
- `http/`: routing, response construction and request parsing.
- `auth/`: anonymous installation identity and existing admin session behavior.
- `presets/`: scanning, catalog, submissions, interactions and moderation.
- `updates/`: existing release defaults, settings and artifact delivery.
- `storage/`: extracted catalog, settings, client and audit queries/object operations.
- `admin/`: pages, listings, batch actions, whitelist and ZIP export.
- `jobs/`: scheduled retention purge.
- `shared/`: pure value, version, time and cryptographic helpers.

Some domain handlers still contain explicit SQL. Finishing that boundary is tracked in the implementation status; this extraction does not claim a complete persistence-layer redesign.

## Configuration and operations

`wrangler.toml` is the checked-in template. `wrangler.local.toml` holds local deployment binding identifiers and is ignored. Existing deploy/migration scripts intentionally target the local configuration; `:dev` scripts select the dev environment. Inspect the target bindings before running a remote migration or deployment.

Existing secret/configuration names include `ADMIN_TOKEN`, optional `ADMIN_LOGIN_KEY`, and `DEVICE_HASH_SECRET`. Keep secrets out of tracked files.

Unexpected request/scheduled failures emit JSON log events with method, path and error type only; they omit headers, request bodies, client IDs and secrets. The existing HTTP 500 response behavior remains for compatibility. Check the target environment's Wrangler observability settings before enabling remote logs or traces.

The catalog generation counter uses the existing `app_settings` table. Migration `0005` adds a content revision and durable staging/cleanup tables for preset R2 objects; apply it before deploying this Worker. Existing migrations `0001`–`0004` are unchanged. Conditional R2 catalog publication prevents an older refactored Worker publication from replacing a newer one. This does not retrofit that guarantee into an older Worker running concurrently. The admin catalog-rebuild endpoint remains the manual recovery mechanism if publication fails after a mutation.

Preset writes now store bytes at unique revision paths and publish the referenced key with a guarded D1 mutation. A failed D1 write leaves a staging record for scheduled cleanup. Replaced or purged keys enter a durable cleanup queue; an R2 delete failure leaves the queue entry for retry. The scheduled job drains both tables and reclaims abandoned staging entries after 24 hours.
Keep the previous Worker deployment available for rollback. Test the current addon against the refactored dev backend before production release. No deployment was performed as part of this local refactor.

## Release publication and recovery

Artifact uploads retain the existing 128 MiB limit, but stream through a SHA-256 digest and 5 MiB multipart buffers. Failed/oversized uploads cancel the input and abort multipart state. Successful uploads receive unique immutable paths under `updates/{version}/artifacts/{upload-id}/CrimsonWeather.addon64`; the download API still exposes the same versioned URL and filename.

The version, artifact path, hash, size and audit entry publish in one D1 batch after R2 completes. Settings reads use one query. A failed publication leaves the previous settings and artifact intact; retrying the upload is safe. Changing release version clears artifact fields in that same transaction, including environment fallbacks. This does not make unrelated concurrent release edits a single operation: the last successful publication wins.

Completed but unpublished objects and superseded release objects are retained for recovery. Do not apply an indiscriminate expiry rule to the active artifact prefix. To reclaim them, pause release publishing, read the active `update.addonR2Key`, retain the active artifact and any rollback artifacts, then remove only explicitly reviewed unreferenced objects. Incomplete multipart uploads are aborted on handled failures; platform termination may still require the bucket's incomplete-multipart lifecycle cleanup. No cleanup policy or production resources were changed here.

The previous Worker can read the unchanged settings keys and download the new object paths. Its old upload handler can still overwrite its own legacy fixed paths, and its settings reader can revive environment defaults after an explicit clear; those old behaviors are not fixed by rolling back code.
