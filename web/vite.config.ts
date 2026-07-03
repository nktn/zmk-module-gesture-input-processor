import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

// https://vite.dev/config/
export default defineConfig({
  base: process.env.VITE_BASE ?? "/zmk-module-gesture-input-processor/",
  plugins: [react()],
});
