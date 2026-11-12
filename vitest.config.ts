import path from 'node:path';
import { configDefaults, defineConfig } from 'vitest/config';

const root = process.cwd();

export default defineConfig({
  test: { exclude: [...configDefaults.exclude, 'tests/e2e/**'] },
  resolve: {
    alias: [
      { find: /^.*\/lib\/ui\/icons$/, replacement: path.resolve(root, 'lib/ui/icons.stub.ts') },
      { find: 'react-native-svg', replacement: path.resolve(root, 'lib/ui/svg.stub.tsx') },
    ],
  },
});
