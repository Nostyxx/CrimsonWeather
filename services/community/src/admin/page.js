// Admin page assets, bundled as text by build.mjs (see the ?text plugin).
import html from "./ui/index.html?text";
import css from "./ui/app.css?text";
import js from "./ui/app.js?text";

export const ASSETS = {
  "/": { body: html, type: "text/html; charset=utf-8" },
  "/index.html": { body: html, type: "text/html; charset=utf-8" },
  "/app.css": { body: css, type: "text/css; charset=utf-8" },
  "/app.js": { body: js, type: "text/javascript; charset=utf-8" },
};
