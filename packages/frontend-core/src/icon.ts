/**
 * Attach a class to raw SVG markup from `unplugin-icons`.
 *
 * The icon has to *be* the classed element, not sit inside a classed wrapper: as a flex
 * item a bare `<svg>` contributes its own height and baseline, while a wrapping span
 * contributes a text line box instead, which is a pixel taller and shifts everything
 * laid out against it.
 */
export const icon = (markup: string, className: string): string =>
  markup.replace('<svg', `<svg class="${className}"`)
