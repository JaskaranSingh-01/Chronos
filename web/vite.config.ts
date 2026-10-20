import react from '@vitejs/plugin-react';
import type { ClientRequest, IncomingMessage } from 'node:http';
import { defineConfig } from 'vitest/config';

// `npm run dev` proxies to a chronos-serve on CHRONOS_PORT (default 8080), which checks Host on /api, and Origin on
// /ws and on the Lab's POSTs and DELETEs.
const target = `http://127.0.0.1:${process.env.CHRONOS_PORT ?? '8080'}`;

// Only the dev page's own loopback origin is rewritten; any other keeps its Origin and is refused.
const ownOrigin = (request: ClientRequest, incoming: IncomingMessage) => {
  const { origin, host } = incoming.headers;
  if (origin === `http://${host}` && /^(localhost|127\.0\.0\.1):\d+$/.test(host ?? '')) {
    request.setHeader('Origin', target);
  }
};

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/api': { target, changeOrigin: true, configure: (proxy) => proxy.on('proxyReq', ownOrigin) },
      '/ws': { target, ws: true, changeOrigin: true, configure: (proxy) => proxy.on('proxyReqWs', ownOrigin) },
    },
  },
  test: { include: ['src/**/*.test.ts'] },
});
