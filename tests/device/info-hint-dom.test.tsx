import React from 'react';
const { renderToStaticMarkup } = require('react-dom/server') as { renderToStaticMarkup: (node: React.ReactNode) => string };
import { expect, it, vi } from 'vitest';
import { TamaguiProvider } from 'tamagui';
import config from '../../tamagui.config';
import { DescriptionHint } from '../../components/SectionHeader';

it('renders the info trigger as accessible DOM without variant attributes', () => {
  const warnings: string[] = [];
  const error = vi.spyOn(console, 'error').mockImplementation((...args) => warnings.push(args.join(' ')));
  try {
    const markup = renderToStaticMarkup(
      <TamaguiProvider config={config} defaultTheme="light">
        <DescriptionHint description="DOM boundary help" />
      </TamaguiProvider>,
    );
    expect(markup).toContain('aria-label="Lihat informasi"');
    expect(markup).not.toMatch(/\s(?:bordered|elevate)="(?:true|false)"/);
    expect(warnings.join('\n')).not.toMatch(/Received `true` for a non-boolean attribute/);
  } finally {
    error.mockRestore();
  }
});
