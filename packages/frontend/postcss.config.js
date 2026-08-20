import { purgeCSSPlugin } from '@fullhuman/postcss-purgecss'
import cssnano from 'cssnano'

// The full Bootstrap build is compiled and then cut down to what the markup actually
// uses. Mithril writes classes into selector strings (`.btn.btn-gradido`), so the
// extractor has to accept the same characters a CSS selector may contain.
const purge = purgeCSSPlugin({
  content: ['./index.html', './src/**/*.ts', '../frontend-core/src/**/*.ts'],
  defaultExtractor: (content) => content.match(/[\w-/:]+(?<!:)/g) || [],
  safelist: {
    standard: ['show', 'fade', 'collapsing', 'is-valid', 'is-invalid'],
    deep: [/^toast/, /^carousel/, /^collapse/, /^dropdown/, /^btn-close/],
  },
})

export default {
  plugins:
    process.env.NODE_ENV === 'production'
      ? [purge, cssnano({ preset: ['default', { discardComments: { removeAll: true } }] })]
      : [],
}
