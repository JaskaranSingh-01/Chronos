import { defineConfig } from '@playwright/test';

// Each test starts its own chronos-serve on the fixture (e2e/serve.ts), so the tests share no session.
export default defineConfig({
  testDir: 'e2e',
  forbidOnly: !!process.env.CI,
  use: { browserName: 'chromium', viewport: { width: 1440, height: 860 }, trace: 'retain-on-failure' },
});
