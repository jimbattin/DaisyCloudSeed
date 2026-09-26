import preact from '@preact/preset-vite';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [preact()],
  base: './',
  // localhost is a secure context, which Web MIDI SysEx requires. fs.allow lets the app
  // import ../presets.toml?raw from outside editor/.
  server: { host: 'localhost', port: 5174, strictPort: true, fs: { allow: ['..'] } },
  preview: { host: 'localhost', port: 4174, strictPort: true },
  test: { include: ['src/**/*.test.{ts,tsx}'], environment: 'node' },
});
