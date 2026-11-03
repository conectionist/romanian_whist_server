import { defineConfig, devices } from "@playwright/test";

const server = process.env.WHIST_SERVER ?? "../build/server/app/whist_server";

export default defineConfig({
  testDir: "tests/e2e",
  workers: 1,
  fullyParallel: false,
  forbidOnly: true,
  use: {
    baseURL: "http://127.0.0.1:8090",
  },
  projects: [
    { name: "chromium", use: { ...devices["Desktop Chrome"] } },
  ],
  webServer: {
    command: `${server} --config tests/e2e/server.json`,
    url: "http://127.0.0.1:8090/",
    reuseExistingServer: false,
    timeout: 30_000,
  },
});
