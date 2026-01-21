import { defineConfig } from 'astro/config';
import starlight from '@astrojs/starlight';
import react from '@astrojs/react';
import tailwind from '@astrojs/tailwind';

export default defineConfig({
  site: 'https://halgari.github.io',
  base: '/mnemonicdb',
  vite: {
    optimizeDeps: {
      exclude: ['@electric-sql/pglite'],
    },
    worker: {
      format: 'es',
    },
  },
  integrations: [
    starlight({
      title: 'MnemonicDB',
      description: 'Immutable temporal tuplestore built on PGLite',
      social: {
        github: 'https://github.com/halgari/mnemonicdb',
      },
      customCss: [
        './src/styles/synthwave.css',
      ],
      sidebar: [
        {
          label: 'Getting Started',
          items: [
            { label: 'Overview', link: '/' },
          ],
        },
        {
          label: 'Concepts',
          autogenerate: { directory: 'concepts' },
        },
        {
          label: 'API Reference',
          autogenerate: { directory: 'api' },
        },
        {
          label: 'Examples',
          autogenerate: { directory: 'examples' },
        },
      ],
    }),
    react(),
    tailwind({
      applyBaseStyles: false,
    }),
  ],
});
