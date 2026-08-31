// Renders every template, locale and branch variant straight from pug, with test
// values containing everything that can go wrong.
//
// This is the only place that knows how a document is produced from the pug
// sources. `tools/snapshots.mjs` writes what it returns into tests/__snapshots__,
// and `tests/snapshots.test.mjs` asserts that the two still agree -- which is what
// makes a pug upgrade or an edited template visible as a diff instead of as a
// silently different mail.

import fs from 'fs'
import path from 'path'
import pug from 'pug'
import { LOCALE_DIR, LOCALES, TEMPLATE_ROOT, TEMPLATES } from './manifest.mjs'
import { combosOf, fixture } from './variants.mjs'

export { fixture }

/** Where a document lives under tests/__snapshots__, and what tools/dump.c calls it. */
export const snapshotName = (template, locale, combo, kind) =>
  path.join(template, `${locale}.${combo}.${kind}`)
export const dumpName = (template, locale, combo, kind) =>
  `${template}.${locale}.${combo}.${kind}`

const flatten = (o, p = '') =>
  Object.entries(o).reduce((acc, [k, v]) => {
    if (v && typeof v === 'object') Object.assign(acc, flatten(v, `${p + k}.`))
    else acc[p + k] = v
    return acc
  }, {})

const catalogs = Object.fromEntries(
  LOCALES.map((l) => [
    l,
    flatten(JSON.parse(fs.readFileSync(path.join(LOCALE_DIR, `${l}.json`), 'utf8'))),
  ]),
)

function makeT(locale) {
  const cat = catalogs[locale]
  return (key, params) => {
    let s = cat[key] ?? catalogs.en[key]
    if (params)
      s = s.replace(/\{([A-Za-z0-9_]+)\}/g, (m, k) => (k in params ? String(params[k]) : m))
    return s
  }
}

function pugVars(fn) {
  return /\(function \(([^)]*)\)/
    .exec(fn.toString())[1]
    .split(',')
    .map((s) => s.trim())
    .filter(Boolean)
}

// __slot marker -> fixture value (a sentinel in extract.mjs)
function materialize(v) {
  if (v && typeof v === 'object' && '__slot' in v) return fixture(v.__slot)
  if (v && typeof v === 'object')
    return Object.fromEntries(Object.entries(v).map(([k, x]) => [k, materialize(x)]))
  return v
}

/**
 * Every document, in a fixed order: template, then kind, then locale, then variant.
 * Yields `{ template, locale, combo, kind, snapshot, dump, text }`.
 */
export function* renderAll() {
  for (const [template, spec] of Object.entries(TEMPLATES)) {
    const conditions = spec.conditions ?? []
    const flags = spec.flags ?? []
    const combos = combosOf(template)

    for (const kind of ['html', 'subject']) {
      const file = path.join(TEMPLATE_ROOT, template, kind === 'html' ? 'html.pug' : 'subject.pug')
      const fn = pug.compileFile(file, { basedir: TEMPLATE_ROOT, filename: file })
      const vars = pugVars(fn).filter((v) => v !== 't' && v !== 'locale' && !flags.includes(v))

      for (const locale of LOCALES) {
        for (const [combo, cases] of combos.entries()) {
          const locals = { t: makeT(locale), locale }
          for (const v of vars) locals[v] = fixture(v)
          for (const f of flags) locals[f] = false
          cases.forEach((c, i) => Object.assign(locals, materialize(conditions[i].cases[c].locals ?? {})))

          let text = fn(locals)
          // The subject is plain text, so entities are decoded again -- the same
          // thing tools/extract.mjs does for the subject programs.
          if (kind === 'subject')
            text = text
              .trim()
              .replace(
                /&(amp|lt|gt|quot|#39);/g,
                (m, e) => ({ amp: '&', lt: '<', gt: '>', quot: '"', '#39': "'" })[e],
              )

          yield {
            template,
            locale,
            combo,
            kind,
            snapshot: snapshotName(template, locale, combo, kind),
            dump: dumpName(template, locale, combo, kind),
            text,
          }
        }
      }
    }
  }
}
