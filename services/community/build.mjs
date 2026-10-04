// Bundles both Workers into dist/ (used by the tests and for deployment).
import { build } from "esbuild";
import { readFile } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const root = dirname(fileURLToPath(import.meta.url));

// `import x from "./file.js?text"` imports the file's contents as a string
// (used for the admin page's HTML/CSS/browser script).
const textImports = {
  name: "text-imports",
  setup(b) {
    b.onResolve({ filter: /\?text$/ }, (args) => ({
      path: resolve(args.resolveDir, args.path.slice(0, -"?text".length)),
      namespace: "text-file",
    }));
    b.onLoad({ filter: /.*/, namespace: "text-file" }, async (args) => ({
      contents: await readFile(args.path, "utf8"),
      loader: "text",
      watchFiles: [args.path],
    }));
  },
};

export async function buildWorkers() {
  for (const name of ["public", "admin"]) {
    await build({
      entryPoints: [join(root, "src", name, "index.js")],
      outfile: join(root, "dist", name, "index.js"), // one folder per Worker (deployed as-is)
      bundle: true,
      format: "esm",
      target: "es2022",
      platform: "neutral",
      plugins: [textImports],
      logLevel: "warning",
    });
  }
}

if (process.argv[1] === fileURLToPath(import.meta.url)) await buildWorkers();
