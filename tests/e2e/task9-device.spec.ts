import { expect, test, type Page } from '@playwright/test';

const devicePath = '/devices/ui-v30-test';
const runtimeErrors = new WeakMap<Page, string[]>();
test.beforeEach(async ({ page }) => {
  const errors: string[] = [];
  runtimeErrors.set(page, errors);
  page.on('pageerror', error => errors.push(error.message));
});
test.afterEach(async ({ page }) => {
  expect(runtimeErrors.get(page) ?? []).toEqual([]);
});

async function protectPage(page: Page) {
  await page.addInitScript(() => {
    const bluetooth = (navigator as Navigator & { bluetooth?: { requestDevice: () => Promise<never> } }).bluetooth;
    if (bluetooth) Object.defineProperty(bluetooth, 'requestDevice', { configurable: true, value: () => Promise.reject(new Error('E2E safety guard: Bluetooth chooser disabled')) });
  });
  await page.route('**/*', async route => {
    const request = route.request();
    const target = new URL(request.url());
    if (target.origin !== 'http://127.0.0.1:18772') return route.abort('blockedbyclient');
    if (!['GET', 'HEAD', 'OPTIONS'].includes(request.method())) return route.abort('blockedbyclient');
    return route.continue();
  });
}

async function ready(page: Page) {
  await protectPage(page);
  await page.goto(devicePath, { waitUntil: 'domcontentloaded' });
  await expect(page.getByRole('region', { name: 'Alat perangkat' })).toBeVisible({ timeout: 20_000 });
}

async function assertNoOverflow(page: Page) {
  const inspect = () => page.evaluate(() => ({
    viewport: document.documentElement.clientWidth,
    scroll: document.documentElement.scrollWidth,
    badChildren: [...document.querySelectorAll('body *')].filter(el => {
      const r = el.getBoundingClientRect();
      return r.width > 0 && (r.left < -1 || r.right > document.documentElement.clientWidth + 1);
    }).slice(0, 5).map(el => ({ tag: el.tagName, text: el.textContent?.slice(0, 60), left: el.getBoundingClientRect().left, right: el.getBoundingClientRect().right })),
  }));
  let sizes = await inspect();
  await expect.poll(async () => {
    sizes = await inspect();
    return sizes.scroll <= sizes.viewport && sizes.badChildren.length === 0;
  }, { message: 'layout should settle without overflow or out-of-viewport children' }).toBe(true);
  expect(sizes.scroll, JSON.stringify(sizes)).toBeLessThanOrEqual(sizes.viewport);
  expect(sizes.badChildren, JSON.stringify(sizes.badChildren)).toEqual([]);
}

test('device tools, typography, touch targets and responsive layout', async ({ page, isMobile }) => {
  const pageErrors: string[] = [];
  page.on('pageerror', error => pageErrors.push(error.message));
  await ready(page);
  const region = page.getByRole('region', { name: 'Alat perangkat' });
  await expect(region.getByRole('button', { name: 'Konfigurasi Wi-Fi' })).toBeVisible();
  await expect(region.getByRole('button', { name: 'Debug' })).toBeVisible();
  await expect(region.getByRole('group', { name: 'Sensor' })).toBeVisible();
  await expect(region.getByRole('group', { name: 'Kalibrasi' })).toBeVisible();
  await expect(region.getByRole('region', { name: 'Diagnostik sensor' })).toBeVisible();
  const title = region.locator('text=Diagnostik dan kalibrasi sensor');
  await expect(title).toHaveCSS('font-size', '19px');
  await expect(title).toHaveCSS('font-weight', '700');
  const cardTitle = region.getByText('Sensor', { exact: true });
  await expect(cardTitle).toHaveCSS('font-size', '16px');
  await expect(cardTitle).toHaveCSS('font-weight', '700');
  const buttons = region.getByRole('button');
  for (const button of await buttons.all()) {
    const box = await button.boundingBox();
    if (box) expect(Math.min(box.width, box.height)).toBeGreaterThanOrEqual(44);
  }
  await expect(region.getByRole('button', { name: 'Konfigurasi Wi-Fi' })).toHaveCSS('min-height', '70px');
  const groupBoxes = await Promise.all(['Sensor', 'Diagnostik sensor'].map(async name => region.getByRole(name === 'Sensor' ? 'group' : 'region', { name }).boundingBox()));
  if (isMobile) expect(groupBoxes[0]!.y).toBeLessThan(groupBoxes[1]!.y);
  else expect(groupBoxes[0]!.x).toBeLessThan(groupBoxes[1]!.x);
  await assertNoOverflow(page);
  expect(pageErrors).toEqual([]);
});

test('sensor selection supports keyboard on desktop and touch on mobile', async ({ page, isMobile }) => {
  await ready(page);
  const selector = page.getByRole('combobox', { name: 'Pilih sensor' });
  if (isMobile) {
    await selector.tap();
    const option = page.getByRole('option', { name: 'Berat · Anorganik (HX711)', exact: true });
    await expect(option).toBeVisible();
    const box = await option.boundingBox();
    expect(box && Math.min(box.width, box.height)).toBeGreaterThanOrEqual(44);
    await option.tap();
  } else {
    await selector.click();
    await page.keyboard.press('Home');
    await page.keyboard.press('ArrowDown');
    await page.keyboard.press('Enter');
  }
  await expect(selector).toContainText('Anorganik');
});

test('Wi-Fi panel opens and closes without revealing credentials', async ({ page }) => {
  await ready(page);
  await page.getByRole('button', { name: 'Konfigurasi Wi-Fi' }).click();
  await expect(page.getByRole('dialog', { name: 'Pengaturan Wi-Fi' })).toBeVisible();
  await expect(page.getByRole('button', { name: 'Tutup pengaturan Wi-Fi' })).toBeVisible();
  await expect(page.getByLabel('Wi-Fi password')).toHaveCount(0);
  await page.getByRole('button', { name: 'Tutup pengaturan Wi-Fi' }).click();
  await expect(page.getByRole('dialog', { name: 'Pengaturan Wi-Fi' })).toHaveCount(0);
});

test('debug panel hides unless explicitly opened and token input is memory-only password', async ({ page }) => {
  await ready(page);
  await expect(page.getByLabel('Operator bearer token (memory only)')).toHaveCount(0);
  await page.getByRole('button', { name: 'Debug' }).click();
  await expect(page.getByRole('button', { name: 'Sembunyikan debug' })).toBeVisible();
  await page.getByRole('button', { name: 'HTTPS', exact: true }).click();
  const token = page.getByLabel('Operator bearer token (memory only)');
  await expect(token).toHaveAttribute('type', 'password');
  await expect(token).toHaveValue('');
  await page.getByRole('button', { name: 'Sembunyikan debug' }).click();
  await expect(token).toHaveCount(0);
});

test('calibration wizard presents safe stages and closes without invoking operations', async ({ page, isMobile }, testInfo) => {
  await ready(page);
  const coarse = await page.evaluate(() => matchMedia('(pointer: coarse)').matches);
  if (isMobile || coarse) await page.getByRole('button', { name: /^Kalibrasi hx711_organic$/ }).tap();
  else await page.getByRole('button', { name: /^Kalibrasi hx711_organic$/ }).click();
  const dialog = page.getByRole('dialog', { name: 'Wizard kalibrasi sensor' });
  await expect(dialog).toBeVisible();
  await expect(dialog.getByText('Kriteria & Capture')).toBeVisible();
  await expect(dialog.getByText('Validasi & Simpan')).toBeVisible();
  const info = page.getByRole('button', { name: 'Info tahap Kriteria & Capture' });
  if (isMobile || coarse) {
    await info.tap();
    const tooltip = page.locator('[role="tooltip"][data-state="open"]');
    await expect(tooltip).toBeVisible();
    await expect.poll(() => tooltip.evaluate(element => getComputedStyle(element).opacity)).toBe('1');
    const tooltipBox = await tooltip.boundingBox();
    const viewport = page.viewportSize()!;
    expect(tooltipBox!.x).toBeGreaterThanOrEqual(0);
    expect(tooltipBox!.y).toBeGreaterThanOrEqual(0);
    expect(tooltipBox!.x + tooltipBox!.width).toBeLessThanOrEqual(viewport.width);
    expect(tooltipBox!.y + tooltipBox!.height).toBeLessThanOrEqual(viewport.height);
    await testInfo.attach('mobile-calibration-tooltip.png', { body: await page.screenshot(), contentType: 'image/png' });
    await info.tap();
    await expect(page.locator('[role="tooltip"][data-state="open"]')).toHaveCount(0);
    await info.tap();
  } else {
    await info.hover();
  }
  const tooltip = page.locator('[role="tooltip"][data-state="open"]');
  await expect(tooltip).toBeVisible();
  await expect.poll(() => tooltip.evaluate(element => getComputedStyle(element).opacity)).toBe('1');
  const tooltipBox = await tooltip.boundingBox();
  const viewport = page.viewportSize()!;
  expect(tooltipBox!.x).toBeGreaterThanOrEqual(0);
  expect(tooltipBox!.y).toBeGreaterThanOrEqual(0);
  expect(tooltipBox!.x + tooltipBox!.width).toBeLessThanOrEqual(viewport.width);
  expect(tooltipBox!.y + tooltipBox!.height).toBeLessThanOrEqual(viewport.height);
  if (!isMobile && !coarse) {
    await testInfo.attach('desktop-calibration-tooltip.png', { body: await page.screenshot(), contentType: 'image/png' });
    await page.mouse.move(0, 0);
    await info.focus();
    await expect(tooltip).toBeVisible();
    await page.keyboard.press('Tab');
    await expect.poll(() => info.evaluate(element => document.activeElement !== element)).toBe(true);
    await page.keyboard.press('Escape');
    await expect(tooltip).toHaveCount(0);
    await page.mouse.move(0, 0);
    await info.focus();
    await expect(tooltip).toBeVisible();
  }
  await page.keyboard.press('Escape');
  await expect(tooltip).toHaveCount(0);
  await page.getByRole('button', { name: 'Tutup wizard kalibrasi' }).click();
  await expect(dialog).toHaveCount(0);
});

test('responsive boundaries retain containment at 710/711 and 425/426 pixels', async ({ page }) => {
  await ready(page);
  for (const width of [710, 711, 425, 426]) {
    await page.setViewportSize({ width, height: 844 });
    await assertNoOverflow(page);
    const cards = await Promise.all(['Sensor', 'Diagnostik sensor'].map(name => page.getByRole(name === 'Sensor' ? 'group' : 'region', { name }).boundingBox()));
    if (width <= 710) expect(cards[1]!.y).toBeGreaterThan(cards[0]!.y);
    else expect(cards[1]!.x).toBeGreaterThan(cards[0]!.x);
    const card = await page.getByRole('group', { name: 'Kalibrasi' }).boundingBox();
    expect(card!.x).toBeGreaterThanOrEqual(0);
    expect(card!.x + card!.width).toBeLessThanOrEqual(width + 1);
  }
});

test('reduced motion disables control and tooltip transitions; HTTPS wizard keeps secrets blank', async ({ page }) => {
  await ready(page);
  await page.emulateMedia({ reducedMotion: 'no-preference' });
  await page.getByRole('button', { name: 'Kalibrasi melalui HTTPS' }).click();
  await page.getByRole('button', { name: /^Kalibrasi hx711_organic$/ }).click();
  const dialog = page.getByRole('dialog', { name: 'Wizard kalibrasi sensor' });
  const hint = dialog.getByRole('button', { name: 'Info tahap Kriteria & Capture' });
  const normalControl = await hint.evaluate(element => getComputedStyle(element).transitionDuration);
  await page.emulateMedia({ reducedMotion: 'reduce' });
  const reducedControl = await hint.evaluate(element => getComputedStyle(element).transitionDuration);
  expect(reducedControl.split(',').every(value => parseFloat(value) === 0)).toBe(true);
  expect(normalControl).not.toBe(reducedControl);
  await hint.focus();
  const tooltip = page.locator('[role="tooltip"][data-state="open"]');
  await expect(tooltip).toBeVisible();
  const reducedTooltip = await tooltip.evaluate(element => getComputedStyle(element).transitionDuration);
  expect(reducedTooltip.split(',').every(value => parseFloat(value) === 0)).toBe(true);
  const passwords = dialog.locator('input[type="password"]');
  await expect(passwords).toHaveCount(2);
  await expect(passwords.nth(0)).toHaveValue('');
  await expect(passwords.nth(1)).toHaveValue('');
});
