import { defineConfig } from 'vite'
import { resolve } from 'node:path'
import Components from 'unplugin-vue-components/vite'
import Icons from 'unplugin-icons/vite'
import IconsResolve from 'unplugin-icons/resolver'
import EnvironmentPlugin from 'vite-plugin-environment'
import { createHtmlPlugin } from 'vite-plugin-html'
import pluginPurgeCss from '@mojojoejo/vite-plugin-purgecss'
import { schema } from './src/config/schema'
import { validate, browserUrls } from 'shared/config'

import dotenv from 'dotenv'

dotenv.config() // load env vars from .env

import { CONFIG } from  './src/config'
/*
 // Check config
validate(schema, CONFIG)
 // make sure that all urls used in browser have the same protocol to prevent mixed content errors
validate(browserUrls, [
  CONFIG.ADMIN_AUTH_URL,
  CONFIG.COMMUNITY_URL,
  CONFIG.COMMUNITY_REGISTER_URL,
  CONFIG.TRPC_URI,
  CONFIG.FRONTEND_MODULE_URL,
])
*/
export default defineConfig({
  server: {
    host: CONFIG.FRONTEND_MODULE_HOST, // '0.0.0.0',
    port: CONFIG.FRONTEND_MODULE_PORT, // 3000,
    fs: {
      strict: true,
    },
  },
  plugins: [
    createHtmlPlugin({
      minify: CONFIG.PRODUCTION === true,
      inject: {
        data: {
          VITE_META_TITLE_DE: CONFIG.META_TITLE_DE,
          VITE_META_TITLE_EN: CONFIG.META_TITLE_EN,
          VITE_META_DESCRIPTION_DE: CONFIG.META_DESCRIPTION_DE,
          VITE_META_DESCRIPTION_EN: CONFIG.META_DESCRIPTION_EN,
          VITE_META_KEYWORDS_DE: CONFIG.META_KEYWORDS_DE,
          VITE_META_KEYWORDS_EN: CONFIG.META_KEYWORDS_EN,
          VITE_META_AUTHOR: CONFIG.META_AUTHOR,
          VITE_META_URL: CONFIG.META_URL,
        },
      },
    }),
    pluginPurgeCss({
      variables: true,
    }),
    Components({
      resolvers: [IconsResolve()],
      dts: true,
    }),
    Icons({compiler:'raw'}),
    EnvironmentPlugin({
      GMS_ACTIVE: null,
      HUMHUB_ACTIVE: null,
      DEFAULT_PUBLISHER_ID: null,
      PORT: null,
      COMMUNITY_HOST: null,
      URL_PROTOCOL: null,
      COMMUNITY_URL: CONFIG.COMMUNITY_URL,
      TRPC_URI: CONFIG.TRPC_URI, // null,
      ADMIN_AUTH_URL: CONFIG.ADMIN_AUTH_URL, // null,
      COMMUNITY_NAME: null,
      COMMUNITY_REGISTER_PATH: null,
      COMMUNITY_REGISTER_URL: null,
      COMMUNITY_DESCRIPTION: null,
      COMMUNITY_SUPPORT_MAIL: null,
      META_URL: null,
      META_TITLE_DE: null,
      META_TITLE_EN: null,
      META_DESCRIPTION_DE: null,
      META_DESCRIPTION_EN: null,
      META_KEYWORDS_DE: null,
      META_KEYWORDS_EN: null,
      META_AUTHOR: null,
    }),
    // commonjs(),
  ],
  css: {
    preprocessorOptions: {
      scss: {
        // additionalData: `@use "@/assets/scss/custom/gradido-custom/color" as *;`,
      },
    },
  },
  build: {
    outDir: resolve(__dirname, './build'),
    chunkSizeWarningLimit: 1600,
    minify: CONFIG.PRODUCTION === true ? 'esbuild' : false,
    sourcemap: false,
  },
})
