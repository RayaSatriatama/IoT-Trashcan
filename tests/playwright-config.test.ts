import { existsSync, mkdtempSync, mkdirSync, rmSync, writeFileSync } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { afterEach, describe, expect, it, vi } from 'vitest';

const originalEnv = { ...process.env };
let tempRoot: string;

afterEach(() => {
  for (const key of Object.keys(process.env)) if (!(key in originalEnv)) delete process.env[key];
  Object.assign(process.env, originalEnv);
  vi.resetModules();
  if (tempRoot) rmSync(tempRoot, { recursive: true, force: true });
});

async function loadConfig(output: string) {
  process.env.SIGAP_E2E_ORIGIN = 'http://127.0.0.1:18772';
  process.env.SIGAP_E2E_OUTPUT = output;
  process.env.SIGAP_E2E_BROWSER = process.execPath;
  vi.resetModules();
  return import('../playwright.config');
}

describe('Playwright output directory guard', () => {
  it('rejects filesystem root without creating or clearing output', async () => {
    await expect(loadConfig(path.parse(process.cwd()).root)).rejects.toThrow(/dedicated|empty/i);
  });

  it('rejects a nonempty output directory before reporters can overwrite evidence', async () => {
    tempRoot = mkdtempSync(path.join(os.tmpdir(), 'sigap-playwright-config-'));
    const output = path.join(tempRoot, 'existing');
    mkdirSync(output);
    writeFileSync(path.join(output, 'preserve.txt'), 'keep');
    await expect(loadConfig(output)).rejects.toThrow(/dedicated|empty/i);
    expect(await import('node:fs/promises').then(fs => fs.readFile(path.join(output, 'preserve.txt'), 'utf8'))).toBe('keep');
  });

  it('accepts a fresh dedicated child of an existing artifact root', async () => {
    tempRoot = mkdtempSync(path.join(os.tmpdir(), 'sigap-playwright-config-'));
    const output = path.join(tempRoot, 'new-run');
    await expect(loadConfig(output)).resolves.toBeDefined();
    expect(existsSync(output)).toBe(false);
  });

  it('accepts an existing empty dedicated output directory', async () => {
    tempRoot = mkdtempSync(path.join(os.tmpdir(), 'sigap-playwright-config-'));
    const output = path.join(tempRoot, 'empty-run');
    mkdirSync(output);
    await expect(loadConfig(output)).resolves.toBeDefined();
  });
});
