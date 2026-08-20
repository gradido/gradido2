import { resolve } from 'node:path'
import dotenv from 'dotenv'
import Icons from 'unplugin-icons/vite'
import { defineConfig } from 'vite'
import EnvironmentPlugin from 'vite-plugin-environment'
import { createHtmlPlugin } from 'vite-plugin-html'

dotenv.config()

// Imported after dotenv so the schema sees the values from `.env`.
const { CONFIG, PRODUCTION } = await import('./src/config')

export default defineConfig({
  base: CONFIG.BASE_PATH === '' ? '/' : `${CONFIG.BASE_PATH}/`,

  server: {
    host: '0.0.0.0',
    port: CONFIG.DEV_SERVER_PORT,
  },
  preview: {
    host: '0.0.0.0',
    port: CONFIG.DEV_SERVER_PORT,
  },

  resolve: {
    alias: {
      '@': resolve(import.meta.dirname, './src'),
    },
  },

  css: {
    preprocessorOptions: {
      scss: {
        // Bootstrap 5.3's own SCSS still uses `@import` and the legacy color functions,
        // which Dart Sass warns about 300+ times per build. Silenced by name rather than
        // wholesale, so a deprecation in our own stylesheets is still reported.
        silenceDeprecations: [
          'import',
          'global-builtin',
          'color-functions',
          'if-function',
        ],
      },
    },
  },

  plugins: [
    createHtmlPlugin({
      minify: PRODUCTION,
      inject: {
        data: {
          COMMUNITY_NAME: CONFIG.COMMUNITY_NAME,
          BASE_PATH: CONFIG.BASE_PATH,
        },
      },
    }),
    // `raw` gives the SVG markup as a string, which mithril renders through m.trust —
    // no icon component layer, and no icon that is not on the page. No default size:
    // the icons keep their intrinsic 1.2em and scale with whatever text they sit in,
    // which is what the surrounding line boxes are laid out against.
    Icons({ compiler: 'raw' }),
    EnvironmentPlugin({
      NODE_ENV: CONFIG.NODE_ENV,
      API_BASE_URL: CONFIG.API_BASE_URL,
      BASE_PATH: CONFIG.BASE_PATH,
      COMMUNITY_NAME: CONFIG.COMMUNITY_NAME,
      WEBSITE_URL: CONFIG.WEBSITE_URL,
      DEV_SERVER_PORT: String(CONFIG.DEV_SERVER_PORT),
    }),
  ],

  build: {
    outDir: 'build',
    sourcemap: !PRODUCTION,
    minify: PRODUCTION ? 'esbuild' : false,
    cssMinify: PRODUCTION ? 'esbuild' : false,
    chunkSizeWarningLimit: 600,
  },
})
