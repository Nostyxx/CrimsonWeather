import { bytesOf } from "../shared/values.js";
import { newestPendingUpdateFilter } from "./queries.js";
import { nowIso } from "../shared/time.js";
import { text } from "../http/responses.js";
import { scanPresetIni } from "../presets/scanner.js";

export function crc32(bytes) {
  let c = ~0;
  for (const b of bytes) {
    c ^= b;
    for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1));
  }
  return (~c) >>> 0;
}

export function u16(n) { return [n & 255, (n >>> 8) & 255]; }

export function u32(n) { return [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255]; }

export function zipStore(files) {
  const chunks = [];
  const central = [];
  let offset = 0;
  for (const file of files) {
    const name = bytesOf(file.name);
    const data = typeof file.data === "string" ? bytesOf(file.data) : file.data;
    const crc = crc32(data);
    const local = new Uint8Array([
      ...u32(0x04034b50), ...u16(20), ...u16(0), ...u16(0), ...u16(0), ...u16(0),
      ...u32(crc), ...u32(data.length), ...u32(data.length), ...u16(name.length), ...u16(0)
    ]);
    chunks.push(local, name, data);
    central.push({ name, crc, size: data.length, offset });
    offset += local.length + name.length + data.length;
  }
  const centralStart = offset;
  for (const entry of central) {
    const header = new Uint8Array([
      ...u32(0x02014b50), ...u16(20), ...u16(20), ...u16(0), ...u16(0), ...u16(0), ...u16(0),
      ...u32(entry.crc), ...u32(entry.size), ...u32(entry.size), ...u16(entry.name.length),
      ...u16(0), ...u16(0), ...u16(0), ...u16(0), ...u32(0), ...u32(entry.offset)
    ]);
    chunks.push(header, entry.name);
    offset += header.length + entry.name.length;
  }
  const centralSize = offset - centralStart;
  chunks.push(new Uint8Array([
    ...u32(0x06054b50), ...u16(0), ...u16(0), ...u16(central.length), ...u16(central.length),
    ...u32(centralSize), ...u32(centralStart), ...u16(0)
  ]));
  return new Blob(chunks, { type: "application/zip" });
}

export async function exportPending(env) {
  const { results } = await env.DB.prepare(`SELECT * FROM presets WHERE status='pending' AND deleted_at IS NULL AND ${newestPendingUpdateFilter("presets")} ORDER BY created_at ASC`).all();
  const rows = results || [];
  const files = [{ name: "manifest.json", data: JSON.stringify({ exportedAt: nowIso(), submissions: rows }, null, 2) }];
  for (const row of rows) {
    const object = await env.PRESETS.get(row.r2_key);
    const ini = object ? await object.text() : "";
    const scan = scanPresetIni(ini);
    files.push({ name: `${row.id}/preset.ini`, data: ini });
    files.push({ name: `${row.id}/metadata.json`, data: JSON.stringify(row, null, 2) });
    files.push({ name: `${row.id}/scan.json`, data: JSON.stringify(scan, null, 2) });
  }
  return new Response(zipStore(files), {
    headers: {
      "content-type": "application/zip",
      "content-disposition": "attachment; filename=\"crimson-weather-pending-presets.zip\""
    }
  });
}
