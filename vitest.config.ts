import { defineConfig } from "vitest/config";
import { resolve } from "path";

export default defineConfig({
  resolve: {
    alias: {
      "@": resolve(__dirname, "src"),
      "@lib": resolve(__dirname, "src/lib"),
    },
  },
  test: {
    environment: "happy-dom",
    include: ["tests/**/*.test.ts"],
    setupFiles: ["tests/setup.ts"],
    globals: false,
  },
});
