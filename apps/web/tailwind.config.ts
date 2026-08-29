import type { Config } from 'tailwindcss';

/**
 * The player is a full-bleed dark surface. Any bright chrome around the video
 * shifts your perception of the stream itself, and this UI exists partly to
 * judge image quality - so everything is near-black with one signal colour and
 * a small semantic set for pass/warn/fail against the PRD section 2 targets.
 */
const config: Config = {
  content: ['./src/**/*.{ts,tsx}'],
  theme: {
    extend: {
      colors: {
        void: '#07090b',
        panel: '#11151a',
        edge: '#1e252d',
        ink: '#e6ecf2',
        muted: '#8494a3',
        signal: '#4c8dff',
        good: '#3cbe95',
        warn: '#e0a445',
        bad: '#f2555a',
      },
      fontFamily: {
        mono: ['ui-monospace', 'SFMono-Regular', 'Menlo', 'Consolas', 'monospace'],
      },
      fontSize: {
        hud: ['11px', { lineHeight: '15px' }],
      },
    },
  },
  plugins: [],
};

export default config;
