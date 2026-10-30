import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// In development, /api is proxied to the C++ server. In production the C++
// server serves the built files from dist/ itself, so no proxy is needed.
export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    proxy: { '/api': process.env.API_URL || 'http://localhost:8080' },
  },
  build: {
    rollupOptions: {
      output: {
        // Split the bundle into three files: React, the chart library (Recharts
        // with its d3 / redux dependencies) and the dashboard code. Each stays
        // below Vite's 500 kB chunk warning, and the two library files keep
        // their hashed names (and the browser cache) when only dashboard code
        // changes.
        manualChunks(id) {
          if (!id.includes('node_modules')) return undefined;
          if (/[\\/]node_modules[\\/](react|react-dom|scheduler)[\\/]/.test(id)) return 'react';
          return 'charts';
        },
      },
    },
  },
});
