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
        // Only the dev page's own loopback origin is rewritten; any other keeps its Origin and is refused.
        configure: (proxy) =>
          proxy.on('proxyReqWs', (request, incoming) => {
            const { origin, host } = incoming.headers;
            if (origin === `http://${host}` && /^(localhost|127\.0\.0\.1):\d+$/.test(host ?? '')) {
              request.setHeader('Origin', target);
            }
          }),
      },
    },
  },
  test: { include: ['src/**/*.test.ts'] },
});
