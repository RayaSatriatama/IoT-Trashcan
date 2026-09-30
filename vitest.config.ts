import path from 'node:path';
import { defineConfig } from 'vitest/config';

const root = process.cwd();

export default defineConfig({
  resolve: {
    alias: [
      { find: /^.*\/lib\/ui\/icons$/, replacement: path.resolve(root, 'lib/ui/icons.stub.ts') },
      { find: 'react-native-svg', replacement: path.resolve(root, 'lib/ui/svg.stub.tsx') },
    ],
  },
});
