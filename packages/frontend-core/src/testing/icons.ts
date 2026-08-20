import { mock } from 'bun:test'

/**
 * Stand in for the `~icons/*` modules.
 *
 * Those are virtual: `unplugin-icons` turns each path into raw SVG markup while Vite is
 * running, and nothing resolves them outside it. Importing anything from the
 * `frontend-core` barrel reaches the components that use them, so most component tests
 * need this even when icons are nowhere near what they assert.
 *
 * Call it before importing the module under test — `await import(…)` after, since a
 * static import would be hoisted above the call.
 */
const ICONS = [
  'bi/eye',
  'bi/eye-slash',
  'bi/globe2',
  'bi/caret-down-fill',
  'bi/check',
  'bi/facebook',
  'bi/telegram',
  'bi/twitter-x',
  'bi/youtube',
]

export const stubIcons = (): void => {
  for (const name of ICONS) {
    mock.module(`~icons/${name}`, () => ({ default: `<svg data-icon="${name}"></svg>` }))
  }
}
