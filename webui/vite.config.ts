import { defineConfig } from "vite";
import { svelte } from "@sveltejs/vite-plugin-svelte";

// Fixed, non-hashed output filenames and a single JS chunk (no code
// splitting) - the firmware side embeds the built files individually via
// ESP-IDF's EMBED_FILES, which needs a known, stable file list rather
// than Vite's default content-hashed many-chunk output. See
// docs/decisions/ADR-0025-webui-static-asset-storage.md.
export default defineConfig({
  plugins: [svelte()],
  build: {
    outDir: "dist",
    rollupOptions: {
      output: {
        entryFileNames: "app.js",
        assetFileNames: "app.[ext]",
      },
    },
  },
});
