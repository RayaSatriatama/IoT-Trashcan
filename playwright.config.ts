import { existsSync, readdirSync } from 'node:fs';
import path from 'node:path';
import { defineConfig, devices } from '@playwright/test';

const baseURL = process.env.SIGAP_E2E_ORIGIN;
const outputDir = process.env.SIGAP_E2E_OUTPUT;
const executablePath = process.env.SIGAP_E2E_BROWSER;
if (!baseURL || !outputDir || !executablePath) {
  throw new Error('Set SIGAP_E2E_ORIGIN, SIGAP_E2E_OUTPUT and SIGAP_E2E_BROWSER explicitly.');
}
const resolvedOutputDir = path.resolve(outputDir);
const checkoutRoot = path.resolve(process.cwd());
const isWorker = process.env.TEST_WORKER_INDEX !== undefined;
if (!path.isAbsolute(outputDir) || resolvedOutputDir === path.parse(resolvedOutputDir).root || resolvedOutputDir === checkoutRoot || resolvedOutputDir.startsWith(`${checkoutRoot}${path.sep}`)) {
  throw new Error('SIGAP_E2E_OUTPUT must be a dedicated absolute artifact directory outside the project checkout.');
}
if (!isWorker && existsSync(resolvedOutputDir) && readdirSync(resolvedOutputDir).length > 0) {
  throw new Error('SIGAP_E2E_OUTPUT must be a fresh or empty dedicated artifact directory.');
}
if (!existsSync(executablePath)) throw new Error('SIGAP_E2E_BROWSER must name an existing browser executable.');
const url = new URL(baseURL);
if (url.protocol !== 'http:' || url.hostname !== '127.0.0.1' || url.port !== '18772' || url.username || url.password || url.pathname !== '/' || url.search || url.hash) {
  throw new Error('E2E origin must be the isolated http://127.0.0.1:18772 backend.');
}

export default defineConfig({
  testDir: './tests/e2e',
  outputDir: `${outputDir}/test-results`,
  fullyParallel: false,
  forbidOnly: true,
  retries: 0,
  workers: 1,
  reporter: [['list'], ['json', { outputFile: `${outputDir}/results.json` }], ['html', { outputFolder: `${outputDir}/html`, open: 'never' }]],
  use: { baseURL, browserName: 'chromium', launchOptions: { executablePath }, trace: 'retain-on-failure', screenshot: 'only-on-failure', actionTimeout: 8_000, navigationTimeout: 20_000 },
  projects: [
    { name: 'desktop', use: { ...devices['Desktop Chrome'], viewport: { width: 1920, height: 1080 } } },
    { name: 'mobile', use: { ...devices['iPhone 13'], defaultBrowserType: 'chromium', viewport: { width: 390, height: 844 }, isMobile: true, hasTouch: true } },
  ],
});
