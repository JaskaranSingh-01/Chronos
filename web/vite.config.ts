import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

// `npm run dev` proxies to a chronos-serve on CHRONOS_PORT (default 8080), which checks Host on /api and Origin on /ws.
const target = `http://127.0.0.1:${process.env.CHRONOS_PORT ?? '8080'}`;

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/api': { target, changeOrigin: true },
      '/ws': {
        target,
        ws: true,
        changeOrigin: true,
        configure: (proxy) => proxy.on('proxyReqWs', (request) => request.setHeader('Origin', target)),
      },
    },
  },
  test: { include: ['src/**/*.test.ts'] },
});
