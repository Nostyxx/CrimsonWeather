import { json } from "./responses.js";

export async function readJson(request) {
  try {
    const length = Number(request.headers.get("content-length") || 0);
    if (length > 140000) return null;
    return await request.json();
  } catch {
    return null;
  }
}
