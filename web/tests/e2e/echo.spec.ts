import { expect, test } from "@playwright/test";

test("the page loads from the server, and the echo answers", async ({ page }) => {
  await page.goto("/");
  await expect(page.getByRole("heading", { name: "Romanian Whist" })).toBeVisible();
  await expect(page.locator("#status")).toHaveText("connected");

  await page.getByLabel("Message").fill("hello");
  await page.getByRole("button", { name: "Send" }).click();

  await expect(page.locator("#log li")).toHaveText(["hello"]);
});
