// Renders every pug template once per locale and per branch variant, with
// sentinels where the user data goes, then splits the result into
// [literal, slot, literal, ...]. Output: <out>/ir.json
import pug from 'pug'
import fs from 'fs'
import path from 'path'
import { TEMPLATE_ROOT, LOCALE_DIR, OUT_DIR, LOCALES, TEMPLATES } from './manifest.mjs'

// U+0001 / U+0002 never occur in HTML or CSS, and pug's escaper leaves them
// alone -- so they survive attribute contexts too.
const SOT = String.fromCharCode(1)
const EOT = String.fromCharCode(2)
const sentinel = (n) => SOT + n + EOT
const SPLIT_RE = new RegExp(SOT + '([^' + SOT + EOT + ']*)' + EOT)

// ---------------------------------------------------------------- i18n
const flatten = (o, p = '') =>
  Object.entries(o).reduce((acc, [k, v]) => {
    if (v && typeof v === 'object') Object.assign(acc, flatten(v, p + k + '.'))
    else acc[p + k] = v
    return acc
  }, {})

const catalogs = Object.fromEntries(
  LOCALES.map((l) => [l, flatten(JSON.parse(fs.readFileSync(path.join(LOCALE_DIR, l + '.json'), 'utf8')))]),
)

// i18n is configured with mustacheConfig.tags = ['{','}'] (locales/localization.ts)
function makeT(locale) {
  const cat = catalogs[locale]
  const fallback = catalogs.en
  return (key, params) => {
    let s = cat[key] ?? fallback[key]
    if (s === undefined) throw new Error(`missing translation ${locale}:${key}`)
    if (params) s = s.replace(/\{([A-Za-z0-9_]+)\}/g, (m, k) => (k in params ? String(params[k]) : m))
    return s
  }
}

// ------------------------------------------------ read the variables off pug
// pug compiles to `(function (a, b, t, ...) {...}`, which is the exact list of
// identifiers the template uses, includes and extends included.
function pugVars(fn) {
  const m = /\(function \(([^)]*)\)/.exec(fn.toString())
  return m[1].split(',').map((s) => s.trim()).filter(Boolean)
}

function materialize(v) {
  if (v && typeof v === 'object' && '__slot' in v) return sentinel(v.__slot)
  if (v && typeof v === 'object') return Object.fromEntries(Object.entries(v).map(([k, x]) => [k, materialize(x)]))
  return v
}

const unescapeHtml = (s) =>
  s.replace(/&(amp|lt|gt|quot|#39);/g, (m, e) => ({ amp: '&', lt: '<', gt: '>', quot: '"', '#39': "'" })[e])

function chunkify(html, kind) {
  const parts = html.split(SPLIT_RE) // [lit, slot, lit, slot, ..., lit]
  const ops = []
  for (let i = 0; i < parts.length; i++) {
    if (i % 2 === 0) {
      if (parts[i]) ops.push({ t: 'lit', s: kind === 'text' ? unescapeHtml(parts[i]) : parts[i] })
    } else {
      ops.push({ t: 'slot', name: parts[i], esc: kind === 'text' ? 'raw' : 'html' })
    }
  }
  return ops
}

// ------------------------------------------------ drift check (load-bearing)
// The extractor renders branches exactly the way manifest.mjs describes them.
// If a new `if` appeared in a template, only one branch would silently end up
// in the binary. So: count the branches in the pug sources and hold the
// manifest to that number.
function stripPugComments(src) {
  const lines = src.split('\n')
  const out = []
  let blockIndent = -1
  for (const line of lines) {
    const indent = line.search(/\S/)
    if (blockIndent >= 0) {
      if (indent === -1 || indent > blockIndent) { out.push(''); continue }
      blockIndent = -1
    }
    if (/^\s*\/\//.test(line)) { blockIndent = indent; out.push(''); continue }
    out.push(line)
  }
  return out.join('\n')
}

function pugGraph(file, seen = new Set()) {
  const real = path.resolve(file)
  if (seen.has(real) || !real.endsWith('.pug')) return seen
  seen.add(real)
  const src = fs.readFileSync(real, 'utf8')
  for (const m of src.matchAll(/^\s*(?:include|extends?)\s+(\S+)\s*$/gm)) {
    const dep = m[1].startsWith('/')
      ? path.join(TEMPLATE_ROOT, m[1])
      : path.resolve(path.dirname(real), m[1])
    const ext = path.extname(dep)
    if (ext && ext !== '.pug') continue // include includes/email.css -- raw text, not pug
    pugGraph(ext ? dep : dep + '.pug', seen)
  }
  return seen
}

function countBranchPoints(name) {
  let n = 0
  const where = []
  for (const kind of ['html.pug', 'subject.pug'])
    for (const f of pugGraph(path.join(TEMPLATE_ROOT, name, kind))) {
      const src = stripPugComments(fs.readFileSync(f, 'utf8'))
      for (const m of src.matchAll(/^[ \t]*(else if|if|unless|case|each|while)\b/gm)) {
        n++
        where.push(`${path.relative(TEMPLATE_ROOT, f)}: ${m[1]}`)
      }
    }
  return { n, where }
}

// ------------------------------------------------------------------ build
const cartesian = (arrs) => arrs.reduce((a, b) => a.flatMap((x) => b.map((y) => [...x, y])), [[]])

const ir = { locales: LOCALES, templates: [] }
const problems = []

for (const [name, spec] of Object.entries(TEMPLATES)) {
  const conditions = spec.conditions ?? []
  const flags = spec.flags ?? []
  const combos = cartesian(conditions.map((c) => c.cases.map((_, i) => i)))

  const declared = conditions.reduce((a, c) => a + c.cases.length - 1, 0)
  const found = countBranchPoints(name)
  if (found.n !== declared)
    problems.push(
      `${name}: ${found.n} branch(es) in the pug sources, but ${declared} in the manifest.\n` +
        found.where.map((w) => '      ' + w).join('\n') +
        '\n      -> adjust tools/manifest.mjs (otherwise only one branch ships).',
    )

  const slotNames = new Set()
  const renders = {} // renders[kind][localeIdx][comboIdx] = ops

  for (const kind of ['html', 'subject']) {
    const file = path.join(TEMPLATE_ROOT, name, kind === 'html' ? 'html.pug' : 'subject.pug')
    const fn = pug.compileFile(file, { basedir: TEMPLATE_ROOT, filename: file })
    const vars = pugVars(fn).filter((v) => v !== 't' && v !== 'locale' && !flags.includes(v))
    renders[kind] = []
    for (const locale of LOCALES) {
      const perCombo = []
      for (const combo of combos) {
        // Baseline: every template variable becomes its own sentinel ...
        const locals = { t: makeT(locale), locale: sentinel('locale') }
        for (const v of vars) locals[v] = sentinel(v)
        for (const f of flags) locals[f] = false
        // ... unless the branch case says otherwise.
        combo.forEach((ci, i) => Object.assign(locals, materialize(conditions[i].cases[ci].locals ?? {})))

        const out = fn(locals)
        if (/undefined|\[object Object\]/.test(out))
          problems.push(`${name}/${kind}/${locale}: 'undefined' or '[object Object]' in the output`)
        const ops = chunkify(kind === 'subject' ? out.trim() : out, kind === 'subject' ? 'text' : 'html')
        for (const op of ops) if (op.t === 'slot') slotNames.add(op.name)
        perCombo.push(ops)
      }
      renders[kind].push(perCombo)
    }
  }

  slotNames.delete('locale') // filled in by the renderer, not by the caller
  const slots = [...slotNames].sort()
  ir.templates.push({
    name,
    slots, // -> const char* fields
    flags, // -> bool fields
    conditions: conditions.map((c) => ({ id: c.id, cases: c.cases.map((x) => x.c) })),
    combos,
    renders,
  })
  console.error(
    `  ${name.padEnd(32)} ${combos.length} variant(s) x ${LOCALES.length} locales, ${slots.length} slots`,
  )
}

if (problems.length) {
  console.error('\nERROR:\n' + problems.join('\n'))
  process.exit(1)
}
fs.mkdirSync(OUT_DIR, { recursive: true })
const irPath = path.join(OUT_DIR, 'ir.json')
fs.writeFileSync(irPath, JSON.stringify(ir))
console.error(`\nwrote ${irPath} (${(fs.statSync(irPath).size / 1024).toFixed(0)} KB raw)`)
