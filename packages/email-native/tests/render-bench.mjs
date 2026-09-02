/*
 * The addon's render() against the two things it could be replaced by.
 *
 *   JS ops     the same op list walked in JavaScript. This is the real
 *              alternative: gen/mjml/ir.json is on disk anyway, and a role that
 *              wanted mails from TypeScript could walk it in twenty lines --
 *              tools/preview-page.html already does exactly that.
 *   pug        legacy, for reference. It is not what this package builds any
 *              more; it is what `gradido` still runs, and the number is here so
 *              the comparison to the system being replaced does not disappear.
 *
 * The variant is not assumed: every one is walked and the one matching the
 * addon's output is used, so the timing compares two things already proven to
 * produce the same bytes.
 *
 *   node tests/render-bench.mjs
 *   bun  tests/render-bench.mjs
 */
import fs from 'node:fs'
import { createRequire } from 'node:module'
import path from 'node:path'
import pug from 'pug'
import { LOCALE_DIR, TEMPLATE_ROOT } from '../tools/manifest.mjs'

const require = createRequire(import.meta.url)
const email = require('../index.cjs')

const N = 100_000
const LOCALE = 'de'
const TEMPLATE = 'accountActivation'
const IR = path.join(TEMPLATE_ROOT, '..', 'gen', 'mjml', 'ir.json')

const values = {
  firstName: 'Björn',
  lastName: 'Müller & Söhne',
  activationLink: 'https://gradido.net/activate?code=abcdef0123456789abcdef&t=1',
  hours: '23',
  minutes: '59',
  resendLink: 'https://gradido.net/resend?code=abcdef0123456789abcdef',
}

// --- the same walk, in JS -------------------------------------------------
const escape = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c])

/** ge_render_*, in JavaScript. `locale` is the renderer's, not a caller's. */
const walk = (ops, vals, locale) => {
  let out = ''
  for (const op of ops) {
    if (op.t === 'lit') { out += op.s; continue }
    const v = op.name === 'locale' ? locale : (vals[op.name] ?? '')
    out += op.esc === 'html' ? escape(v) : v
  }
  return out
}

let jsHtml = null
let jsSubject = null
if (fs.existsSync(IR)) {
  const ir = JSON.parse(fs.readFileSync(IR, 'utf8'))
  const t = ir.templates.find((x) => x.name === TEMPLATE)
  const li = ir.locales.indexOf(LOCALE)
  const want = email.render(TEMPLATE, LOCALE, values)
  // Which variant did the addon pick? Ask the documents, not the manifest.
  const vi = t.renders.html[li].findIndex((ops) => walk(ops, values, LOCALE) === want.html)
  if (vi < 0) {
    console.error('no variant of the IR matches what the addon rendered -- rebuild:')
    console.error('  bun run extract:mjml && bun run build')
    process.exit(1)
  }
  jsHtml = t.renders.html[li][vi]
  jsSubject = t.renders.subject[li][vi]
} else {
  console.error(`no ${path.relative(process.cwd(), IR)} -- the JS row needs it: bun run extract:mjml\n`)
}

// --- pug, set up the way gradido's core/src/emails does it ----------------
const flatten = (o, p = '') =>
  Object.entries(o).reduce((acc, [k, v]) => {
    if (v && typeof v === 'object') Object.assign(acc, flatten(v, `${p + k}.`))
    else acc[p + k] = v
    return acc
  }, {})
const catalog = flatten(JSON.parse(fs.readFileSync(path.join(LOCALE_DIR, `${LOCALE}.json`), 'utf8')))
const t = (key, params) => {
  let s = catalog[key]
  if (params) s = s.replace(/\{([A-Za-z0-9_]+)\}/g, (m, k) => (k in params ? String(params[k]) : m))
  return s
}
const pugDir = path.join(TEMPLATE_ROOT, TEMPLATE)
const havePug = fs.existsSync(path.join(pugDir, 'html.pug'))
const htmlFn = havePug ? pug.compileFile(path.join(pugDir, 'html.pug'), { basedir: TEMPLATE_ROOT }) : null
const subjectFn = havePug
  ? pug.compileFile(path.join(pugDir, 'subject.pug'), { basedir: TEMPLATE_ROOT })
  : null
const pugLocals = {
  ...values,
  locale: LOCALE,
  t,
  timeDurationObject: { hours: values.hours, minutes: values.minutes },
}

function time(label, fn) {
  fn() // warm up
  const t0 = process.hrtime.bigint()
  let sink = 0
  for (let i = 0; i < N; i++) sink += fn()
  const ns = Number(process.hrtime.bigint() - t0) / N
  console.log(
    `  ${label.padEnd(30)} ${ns.toFixed(0).padStart(7)} ns/mail   ` +
      `${(1e9 / ns / 1e3).toFixed(0).padStart(6)} k/s   (${(sink / N) | 0} B)`,
  )
  return ns
}

console.log(`${TEMPLATE}/${LOCALE}, ${N} renders, html + subject:`)
const jsNs = jsHtml
  ? time('JS ops over ir.json', () => walk(jsHtml, values, LOCALE).length + walk(jsSubject, values, LOCALE).length)
  : null
const pugNs = havePug
  ? time('pug (legacy, reference)', () => htmlFn(pugLocals).length + subjectFn(pugLocals).length)
  : null
const napiNs = time('addon render()', () => {
  const m = email.render(TEMPLATE, LOCALE, values)
  return m.html.length + m.subject.length
})
const bytesNs = time('addon renderBytes()', () => {
  const m = email.renderBytes(TEMPLATE, LOCALE, values)
  return m.html.length + m.subject.length
})

console.log()
if (jsNs) {
  console.log(`  render()      vs JS ops: ${(jsNs / napiNs).toFixed(2)}x`)
  console.log(`  renderBytes() vs JS ops: ${(jsNs / bytesNs).toFixed(2)}x`)
}
if (pugNs) console.log(`  render()      vs pug:    ${(pugNs / napiNs).toFixed(2)}x   (legacy's renderer, other markup)`)
console.log('  (the C render itself is ~0.6 us -- the rest is the N-API boundary:')
console.log('   25 KB of UTF-8 turned into a JS string, or copied into a Buffer)')

/*
 * The path that never crosses the boundary: render into the arena, format the MIME message
 * from there, hand the buffer to a thread pool thread. No JS string of the document is ever
 * made.
 *
 * The number below is the *synchronous* half: everything send() does before the work leaves
 * for the pool. The relay is a closed port, every promise rejects, and each one is caught the
 * moment it is made; what happens on the pool thread afterwards is measured by
 * tests/mail-bench.mjs instead.
 */
if (email.Mailer) {
  const M = 500
  const mailer = new email.Mailer({
    url: 'smtp://127.0.0.1:1', // refused immediately, so the pool threads are not held
    from: 'noreply@gradido.net',
    timeoutMs: 100,
    /*
     * Wide open, and that is the measurement. index.cjs gates sends at
     * UV_THREADPOOL_SIZE-1, so with the default only the first three do their
     * synchronous work before returning and the other 497 park on a promise --
     * the mean then measures the gate (3.4 us) rather than render + MIME (100 us).
     */
    maxConcurrent: M,
  })
  const pending = []
  const t0 = process.hrtime.bigint()
  for (let i = 0; i < M; i++)
    pending.push(mailer.send('member@example.org', TEMPLATE, LOCALE, values).catch(() => {}))
  const ns = Number(process.hrtime.bigint() - t0) / M
  await Promise.all(pending)
  mailer.close()

  console.log('\n  send(), the synchronous half: render + MIME')
  console.log(
    `  ${''.padEnd(30)} ${ns.toFixed(0).padStart(7)} ns/mail   ` +
      `${(1e9 / ns / 1e3).toFixed(0).padStart(6)} k/s`,
  )
  console.log(`  ${(ns / napiNs).toFixed(1)}x what render() costs, and it is mostly the encodings:`)
  console.log('  quoted-printable over 25 KB of HTML and base64 over 28 KB of inline images.')
  console.log('  A mail on the wire is ~70 KB where the document alone is 25 KB.')
}
