// The branch variants a template has, and the values that select each one.
//
// The combo number is not stored anywhere -- it is the position in the cartesian
// product of the conditions in tools/manifest.mjs, and that is how the generated C
// numbers its variants too (see ge_prep_* in the generated file). Everything that
// speaks about a variant gets the numbering from here, so the pug output, the
// snapshots and both implementations cannot disagree about which one is `de.1`.

import { TEMPLATES } from './manifest.mjs'

/** Every character pug escapes (& < > "), one it does not ('), and a non-ASCII one.
 *  Identical to the fixture in tools/dump.c, so the C output is comparable. */
export const fixture = (name) => `{${name}|&<>"'ä}`

const cartesian = (arrs) => arrs.reduce((a, b) => a.flatMap((x) => b.map((y) => [...x, y])), [[]])

/** One entry per variant, each the case index picked for each condition. */
export const combosOf = (template) =>
  cartesian((TEMPLATES[template].conditions ?? []).map((c) => c.cases.map((_, i) => i)))

/**
 * Flattens a case's pug locals into values the addon takes.
 *
 * A slot is named after the *leaf*: `timeDurationObject: { minutes: S('minutes') }` is
 * one pug object but the slot is `minutes`, which is what extract.mjs read off the
 * templates. Falsy leaves (`null`, `0`) are what makes the pug branch drop out, and
 * an empty string is how the same thing is said to C -- GE_HAS() is false for both.
 */
function assign(values, locals) {
  for (const [key, v] of Object.entries(locals)) {
    if (v && typeof v === 'object' && '__slot' in v) values[v.__slot] = fixture(v.__slot)
    else if (v && typeof v === 'object') assign(values, v)
    else if (typeof v === 'boolean') values[key] = v
    else values[key] = v ? String(v) : ''
  }
}

/**
 * For each variant of `template`, the values to hand `render()` so that it selects
 * exactly that variant. `slots` is the template's slot list as the addon reports it.
 *
 * Index 0 is "everything set", which is case 0 of every condition -- that is what the
 * cartesian product puts first, and what the generated C computes for the same input.
 */
export function variantValues(template, slots) {
  const spec = TEMPLATES[template]
  const conditions = spec.conditions ?? []

  return combosOf(template).map((cases) => {
    const values = Object.fromEntries(slots.map((s) => [s, fixture(s)]))
    for (const f of spec.flags ?? []) values[f] = false
    cases.forEach((c, i) => assign(values, conditions[i].cases[c].locals ?? {}))
    return values
  })
}
