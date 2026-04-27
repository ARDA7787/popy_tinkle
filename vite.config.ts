import { defineConfig } from "vite";
import { crx } from "@crxjs/vite-plugin";
import manifest from "./src/manifest.json" with { type: "json" };
import { resolve } from "path";

export default defineConfig({
  resolve: {
    alias: {
      "@": resolve(__dirname, "src"),
      "@lib": resolve(__dirname, "src/lib"),
    },
  },
  plugins: [crx({ manifest })],
  build: {
    target: "esnext",
    minify: "esbuild",
    sourcemap: true,
    rollupOptions: {
      input: {
        popup: resolve(__dirname, "src/popup/popup.html"),
        dashboard: resolve(__dirname, "src/dashboard/dashboard.html"),
        onboarding: resolve(__dirname, "src/onboarding/onboarding.html"),
        offscreen: resolve(__dirname, "src/offscreen/offscreen.html"),
      },
    },
  },
  server: {
    port: 5173,
    strictPort: true,
    hmr: { port: 5174 },
  },
});
