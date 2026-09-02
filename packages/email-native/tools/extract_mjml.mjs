/*
 * The MJML half of the extractor: templates/<name>.mjml -> ir.json, in the same
 * shape tools/extract.mjs produces from the pug sources, so gen_c.mjs, verify.mjs
 * and the preview all read one format and none of them knows which engine made it.
 *
 *   templates/<name>.mjml ─┐
 *   templates/includes/*   ─┤ branch markers -> one variant
 *   po/<lang>/messages.po  ─┤ {{t:...}}      -> the catalogue string, escaped
 *                          ─┤ {{v:...}}      -> a sentinel
 *                           ▼
 *                        mjml2html
 *                           ▼
 *                   [literal, slot, literal, ...]   the same chunkify as pug's
 *
 * Two rules decide where a thing lives, and both are what keeps the drift check
 * honest:
 *
 *   the TEMPLATE says which branches exist    <!--@if x--> ... <!--@endif-->
 *   the MANIFEST says how C decides them      GE_HAS(v->logo_url)
 *
 * A branch in a template that the manifest does not know is an error, and so is
 * the reverse -- that is the wire tools/manifest.mjs already carries for pug, held
 * to the markers here instead of to a regex over pug keywords.
 *
 *   node tools/extract_mjml.mjs [--out DIR] [--templates DIR] [--po DIR]
 */
import { convert } from 'html-to-text'
import mjml2html from 'mjml'
import fs from 'node:fs'
import path from 'node:path'
import { scanBranches, selectBranches } from './branches.mjs'
import { LOCALES, OUT_DIR, TEMPLATE_ROOT, TEMPLATES, TEXT_OPTIONS_MJML } from './manifest.mjs'

const arg = (name, fallback) => {
  const i = process.argv.indexOf(`--${name}`)
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback
}
const PO_DIR = path.resolve(arg('po', path.join(TEMPLATE_ROOT, '..', 'po')))

// U+0001 / U+0002 never occur in HTML or CSS, and neither MJML nor its minifier
// touches them -- so they survive attribute contexts too. Same pair as extract.mjs.
const SOT = String.fromCharCode(1)
const EOT = String.fromCharCode(2)
const sentinel = (n) => SOT + n + EOT
const SPLIT_RE = new RegExp(`${SOT}([^${SOT}${EOT}]*)${EOT}`)

const escape = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c])

const unescapeHtml = (s) =>
  s.replace(/&(amp|lt|gt|quot|#39|#x27);/g, (m, e) =>
    ({ amp: '&', lt: '<', gt: '>', quot: '"', '#39': "'", '#x27': "'" })[e],
  )

// ------------------------------------------------------------------ catalogues
/** Enough of a .po reader for what json2po.mjs writes; no plurals, none are used. */
const readPo = (file) => {
  const unquote = (line) =>
    line
      .slice(line.indexOf('"') + 1, line.lastIndexOf('"'))
      .replace(/\\(n|t|"|\\)/g, (_, c) => ({ n: '\n', t: '\t', '"': '"', '\\': '\\' })[c])
  const catalogue = {}
  let field = null
  let id = ''
  let str = ''
  const flush = () => {
    if (id) catalogue[id] = str || id // empty msgstr: the source is the translation
    id = ''
    str = ''
  }
  for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
    if (line.startsWith('#')) continue
    if (line.trim() === '') { flush(); continue }
    if (line.startsWith('msgid ')) { flush(); field = 'id'; id = unquote(line); continue }
    if (line.startsWith('msgstr ')) { field = 'str'; str = unquote(line); continue }
    if (line.trimStart().startsWith('"')) {
      if (field === 'id') id += unquote(line)
      else str += unquote(line)
    }
  }
  flush()
  return catalogue
}

const catalogues = Object.fromEntries(
  LOCALES.map((l) => [l, readPo(path.join(PO_DIR, l, 'messages.po'))]),
)

/**
 * Every comment, including the include's own. MJML keeps comments by default and
 * they would ride along in every mail; here they are also what could turn a
 * documented example marker into a real substitution.
 */
const stripComments = (xml) => xml.replace(/<!--[\s\S]*?-->/g, '')

/** camelCase -> snake_case, the spelling the C struct field carries. */
const snake = (s) => s.replace(/([a-z0-9])([A-Z])/g, '$1_$2').toLowerCase()


// ------------------------------------------------------------- {{t:}} / {{v:}}
const T_RE = /\{\{t:([^|{}]+)(?:\|([^{}]*))?\}\}/g
const V_RE = /\{\{v:([A-Za-z0-9_]+)\}\}/g

/**
 * `mode` decides what a catalogue string is allowed to contain on the way out.
 * html: escaped, the way pug's `=` escapes it. text: as written -- the subject is
 * a header field, not markup, and an `&` in a name belongs there as an `&`.
 */
const substitute = (xml, catalogue, mode) => {
  const esc = mode === 'html' ? escape : (s) => s
  return xml
    .replace(V_RE, (_, name) => sentinel(name))
    .replace(T_RE, (_, msgid, args) => {
      const slots = (args || '').split(',').map((s) => s.trim()).filter(Boolean)
      const text = catalogue[msgid]
      if (text === undefined) throw new Error(`no message for ${JSON.stringify(msgid)}`)
      return esc(text).replace(/%(\d+)/g, (m, n) => (slots[n - 1] ? sentinel(slots[n - 1]) : m))
    })
}

// --------------------------------------------------------------------- chunkify
/** Identical to tools/extract.mjs: [literal, slot, literal, ...]. */
function chunkify(text, kind) {
  const parts = text.split(SPLIT_RE)
  const ops = []
  for (let i = 0; i < parts.length; i++) {
    if (i % 2 === 0) {
      if (parts[i]) ops.push({ t: 'lit', s: kind === 'text' ? unescapeHtml(parts[i]) : parts[i] })
    } else {
      ops.push({ t: 'slot', name: parts[i], esc: kind === 'html' ? 'html' : 'raw' })
    }
  }
  return ops
}

// ------------------------------------------------------------------------ build
const cartesian = (arrs) => arrs.reduce((a, b) => a.flatMap((x) => b.map((y) => [...x, y])), [[]])

const ir = { locales: LOCALES, templates: [] }
const problems = []

for (const [name, spec] of Object.entries(TEMPLATES)) {
  const file = path.join(TEMPLATE_ROOT, `${name}.mjml`)
  if (!fs.existsSync(file)) { problems.push(`${name}: no ${name}.mjml`); continue }

  const source = fs.readFileSync(file, 'utf8')
  const found = scanBranches(file, {
    readFile: (f) => fs.readFileSync(f, 'utf8'),
    resolve: (from, to) => path.resolve(path.dirname(from), to),
  })
  const declared = spec.conditions ?? []
  const flags = spec.flags ?? []

  /*
   * The drift check, and the reason the ids are written into the template.
   *
   * The two sides do NOT have to spell a branch the same way: the template names
   * the VARIABLE that drives it (`logoUrl`, what an author writes), the manifest
   * names the branch for C (`logo`) and says how C decides it. What has to line up
   * is the count, the order, the number of cases -- and that the manifest's first
   * case actually tests the variable the template branches on. That last one is
   * what catches a marker pointed at the wrong field.
   */
  const mismatch = []
  if (found.length !== declared.length) mismatch.push('different number of branches')
  else
    found.forEach((b, i) => {
      if (b.cases !== declared[i].cases.length)
        mismatch.push(`${b.id}: ${b.cases} case(s) here, ${declared[i].cases.length} in the manifest`)
      const cond = declared[i].cases[0].c ?? ''
      if (!cond.includes(`v->${snake(b.id)}`))
        mismatch.push(`${b.id}: the manifest decides '${declared[i].id}' with \`${cond}\`, which does not test v->${snake(b.id)}`)
    })
  if (mismatch.length) {
    problems.push(
      `${name}: branches in the template do not match tools/manifest.mjs\n` +
        `      template: ${found.map((b) => `${b.id}(${b.cases})`).join(', ') || 'none'}\n` +
        `      manifest: ${declared.map((c) => `${c.id}(${c.cases.length})`).join(', ') || 'none'}\n` +
        mismatch.map((m) => `      ${m}`).join('\n') +
        '\n      -> adjust the markers or the manifest (otherwise a variant does not ship).',
    )
    continue
  }

  const combos = cartesian(found.map((b) => [...Array(b.cases).keys()]))
  const slotNames = new Set()
  const renders = { html: [], text: [], subject: [] }

  for (const locale of LOCALES) {
    const catalogue = catalogues[locale]
    const perHtml = []
    const perText = []
    const perSubject = []

    for (const combo of combos) {
      const chosen = Object.fromEntries(found.map((b, i) => [b.id, combo[i]]))

      /*
       * A PREPROCESSOR, not a substitution done up front -- and that distinction
       * is load-bearing. MJML resolves mj-include itself, AFTER this file has been
       * handed over, and it runs the preprocessors again on every included file.
       * Doing the work before the call would leave every marker inside
       * includes/ untouched: no salutation, no validity lines, no contribution
       * CTA, and no error to say so.
       *
       * Comments go too. They are documentation for whoever reads the template
       * and have no business being bytes on the wire -- and the header comment of
       * accountActivation.mjml SHOWS example markers, which would otherwise be
       * substituted and invent a slot that no caller fills.
       */
      const prepare = (xml) => substitute(stripComments(selectBranches(xml, chosen)), catalogue, 'html')

      // The subject is <mj-title>: a header field, so it is taken as text and
      // never goes through MJML at all. It is always in the template itself.
      const title = /<mj-title>([\s\S]*?)<\/mj-title>/.exec(stripComments(selectBranches(source, chosen)))
      if (!title) { problems.push(`${name}: no <mj-title> to take the subject from`); break }
      perSubject.push(chunkify(substitute(title[1].trim(), catalogue, 'text'), 'text'))

      let html
      try {
        const r = await mjml2html(source, {
          filePath: file,
          ignoreIncludes: false,
          validationLevel: 'strict',
          preprocessors: [prepare],
          // The preview's own config must not reach the build: it would fill the
          // markers with sample values instead of sentinels.
          useMjmlConfigOptions: false,
          minify: true,
        })
        if (r.errors.length) throw new Error(r.errors.map((e) => e.formattedMessage ?? e).join('; '))
        html = r.html
      } catch (e) {
        problems.push(`${name}/${locale}/${combo.join('')}: ${e.message.split('\n')[0]}`)
        break
      }
      if (html.includes('mj-include denied'))
        problems.push(`${name}: an include was denied -- see accountActivation.mjml on includePath`)
      if (/undefined|\[object Object\]/.test(html))
        problems.push(`${name}/${locale}: 'undefined' or '[object Object]' in the output`)

      const ops = chunkify(html, 'html')
      for (const op of ops) if (op.t === 'slot') slotNames.add(op.name)
      perHtml.push(ops)

      // The plain text alternative comes off the same rendering, sentinels and
      // all -- so the text a receiver without HTML sees is derived from the one
      // document rather than maintained beside it.
      /*
       * Three blank lines in a row are nobody's intention: they are what a SKIPPED
       * block leaves behind. `.socialmedia`, the gold rule and the footer logo are
       * all dropped from the text part, but the blocks around them still ask for a
       * blank line each, and those add up. Collapsing a run to one blank line is
       * the whole of the difference between MJML's text part and pug's, once the
       * wording is equal -- measured over all 270 documents, not assumed.
       */
      const text = convert(html, TEXT_OPTIONS_MJML).replace(/\n{3,}/g, '\n\n').trim()
      perText.push(chunkify(text, 'plain'))
    }
    renders.html.push(perHtml)
    renders.text.push(perText)
    renders.subject.push(perSubject)
  }

  for (const b of found) if (!flags.includes(b.id)) slotNames.add(b.id)
  slotNames.delete('locale')
  const slots = [...slotNames].filter((s) => !flags.includes(s)).sort()

  ir.templates.push({
    name,
    slots,
    flags,
    conditions: declared.map((c) => ({ id: c.id, cases: c.cases.map((x) => x.c) })),
    combos,
    renders,
  })
  console.error(
    `  ${name.padEnd(32)} ${combos.length} variant(s) x ${LOCALES.length} locales, ${slots.length} slots`,
  )
}

if (problems.length) {
  console.error(`\nERROR:\n${problems.join('\n')}`)
  process.exit(1)
}

fs.mkdirSync(OUT_DIR, { recursive: true })
const irPath = path.join(OUT_DIR, 'ir.json')
fs.writeFileSync(irPath, JSON.stringify(ir))
console.error(`\nwrote ${irPath} (${(fs.statSync(irPath).size / 1024).toFixed(0)} KB raw)`)
