// Local preview of both Workers (no Cloudflare involved).
//
//   node scripts/preview.mjs                      empty database
//   node scripts/preview.mjs <backup-folder>      load a backup (d1-prod.sql + r2/)
//
// Admin page:  http://localhost:8788  (signed in as the test admin automatically)
// Public API:  http://localhost:8787  (what the addon talks to)
// Listens on localhost only.
import { createServer } from "node:http";
import { existsSync, readFileSync, readdirSync, statSync } from "node:fs";
import { join, relative, sep } from "node:path";
import { buildWorkers } from "../build.mjs";
import { startWorker } from "../test/harness.mjs";
import { IMPLEMENTATIONS } from "../test/run.mjs";
import { accessJwt } from "../test/access-fixture.mjs";

// Splits an SQL dump into statements, respecting quoted strings.
function splitStatements(sql) {
  const out = [];
  let start = 0;
  let quote = null;
  for (let i = 0; i < sql.length; i++) {
    const c = sql[i];
    if (quote) {
      if (c === quote) {
        if (sql[i + 1] === quote) i++; // escaped quote
        else quote = null;
      }
    } else if (c === "'" || c === '"') {
      quote = c;
    } else if (c === ";") {
      const stmt = sql.slice(start, i).trim();
      if (stmt) out.push(stmt);
      start = i + 1;
    }
  }
  return out;
}

function* walk(dir) {
  for (const name of readdirSync(dir)) {
    const path = join(dir, name);
    if (statSync(path).isDirectory()) yield* walk(path);
    else yield path;
  }
}

async function seed(worker, backup) {
  const inserts = splitStatements(readFileSync(join(backup, "d1-prod.sql"), "utf8"))
    .filter((s) => /^INSERT INTO "(presets|preset_likes|preset_downloads_daily|admin_audit|client_whitelist|app_settings)"/.test(s));
  for (let i = 0; i < inserts.length; i += 200) {
    await worker.db.batch(inserts.slice(i, i + 200).map((s) => worker.db.prepare(s)));
  }
  let files = 0;
  const r2 = join(backup, "r2");
  if (existsSync(r2)) {
    for (const path of walk(r2)) {
      await worker.r2.put(relative(r2, path).split(sep).join("/"), readFileSync(path));
      files++;
    }
  }
  console.log(`Loaded ${inserts.length} rows and ${files} files from ${backup}`);
}

function serve(port, handler) {
  createServer(async (req, res) => {
    const chunks = [];
    for await (const chunk of req) chunks.push(chunk);
    const headers = { ...req.headers };
    delete headers.host;
    delete headers.origin; // the local runtime rejects Origin headers
    const response = await handler(req.url, { method: req.method, headers, body: chunks.length ? Buffer.concat(chunks) : undefined });
    res.writeHead(response.status, Object.fromEntries(response.headers));
    res.end(Buffer.from(await response.arrayBuffer()));
  }).listen(port, "127.0.0.1");
}

await buildWorkers();
const worker = await startWorker(IMPLEMENTATIONS.next);
if (process.argv[2]) await seed(worker, process.argv[2]);

serve(8788, async (path, init) => worker.adminFetch(path, { ...init, headers: { ...init.headers, "cf-access-jwt-assertion": await accessJwt() } }));
serve(8787, (path, init) => worker.fetch(path, init));
console.log("Admin page: http://localhost:8788   Public API: http://localhost:8787   (Ctrl+C to stop)");
