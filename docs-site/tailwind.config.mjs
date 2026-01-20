import starlightPlugin from '@astrojs/starlight-tailwind';

const synthwaveColors = {
  'bg-deep': '#0d0221',
  'bg-surface': '#1a0a2e',
  'bg-elevated': '#2d1b4e',
  'neon-pink': '#ff2a6d',
  'neon-cyan': '#05d9e8',
  'neon-purple': '#d300c5',
  'text-primary': '#e0e0ff',
};

export default {
  content: ['./src/**/*.{astro,html,js,jsx,md,mdx,svelte,ts,tsx,vue}'],
  theme: {
    extend: {
      colors: {
        accent: synthwaveColors['neon-pink'],
        ...synthwaveColors,
      },
      boxShadow: {
        'glow-pink': '0 0 20px rgba(255, 42, 109, 0.5)',
        'glow-cyan': '0 0 20px rgba(5, 217, 232, 0.5)',
        'glow-purple': '0 0 20px rgba(211, 0, 197, 0.5)',
      },
    },
  },
  plugins: [starlightPlugin()],
};
