/*
 * The addon's render() against what gradido does today: pug compiled once and
 * called per mail. Same template, same locale, same values -- and, since
 * `zig build check` proves it, the same bytes out.
 *
 *   node tests/render-bench.mjs
 *   bun  tests/render-bench.mjs
 */
import { createRequire } from 'node:module'
import fs from 'node:fs'
import path from 'node:path'
import pug from 'pug'
import { TEMPLATE_ROOT, LOCALE_DIR } from '../tools/manifest.mjs'

const require = createRequire(import.meta.url)
const email = require('../index.cjs')

const N = 100_000
const LOCALE = 'de'
const TEMPLATE = 'accountActivation'

const values = {
  firstName: 'Björn',
  lastName: 'Müller & Söhne',
  activationLink: 'https://gradido.net/activate?code=abcdef0123456789abcdef&t=1',
  hours: '23',
  minutes: '59',
  resendLink: 'https://gradido.net/resend?code=abcdef0123456789abcdef',
}

// --- pug, set up the way core/src/emails does it
const flatten = (o, p = '') =>
  Object.entries(o).reduce((acc, [k, v]) => {
    if (v && typeof v === 'object') Object.assign(acc, flatten(v, p + k + '.'))
    else acc[p + k] = v
    return acc
  }, {})
const catalog = flatten(JSON.parse(fs.readFileSync(path.join(LOCALE_DIR, LOCALE + '.json'), 'utf8')))
const t = (key, params) => {
  let s = catalog[key]
  if (params) s = s.replace(/\{([A-Za-z0-9_]+)\}/g, (m, k) => (k in params ? String(params[k]) : m))
  return s
}
const htmlFn = pug.compileFile(path.join(TEMPLATE_ROOT, TEMPLATE, 'html.pug'), { basedir: TEMPLATE_ROOT })
const subjectFn = pug.compileFile(path.join(TEMPLATE_ROOT, TEMPLATE, 'subject.pug'), { basedir: TEMPLATE_ROOT })
const pugLocals = { ...values, locale: LOCALE, t, timeDurationObject: { hours: values.hours, minutes: values.minutes } }

function time(label, fn) {
  fn() // warm up
  const t0 = process.hrtime.bigint()
  let sink = 0
  for (let i = 0; i < N; i++) sink += fn()
  const ns = Number(process.hrtime.bigint() - t0) / N
  console.log(`  ${label.padEnd(28)} ${ns.toFixed(0).padStart(7)} ns/mail   ${(1e9 / ns / 1e3).toFixed(0).padStart(6)} k/s   (${sink / N | 0} B)`)
  return ns
}

console.log(`${TEMPLATE}/${LOCALE}, ${N} renders, html + subject:`)
const pugNs = time('pug (what gradido does)', () => htmlFn(pugLocals).length + subjectFn(pugLocals).length)
const napiNs = time('addon render()', () => {
  const m = email.render(TEMPLATE, LOCALE, values)
  return m.html.length + m.subject.length
})
const bytesNs = time('addon renderBytes()', () => {
  const m = email.renderBytes(TEMPLATE, LOCALE, values)
  return m.html.length + m.subject.length
})
console.log(`\n  render()      vs pug: ${(pugNs / napiNs).toFixed(2)}x`)
console.log(`  renderBytes() vs pug: ${(pugNs / bytesNs).toFixed(2)}x`)
console.log('  (the C render itself is ~0.6 us -- the rest is the N-API boundary:')
console.log('   21 KB of UTF-8 turned into a JS string, or copied into a Buffer)')

/* The path that never crosses it: render into the arena and hand the pointer
 * straight to the mailer. Workers off, so this measures render + enqueue and
 * nothing goes on a socket. */
if (email.Mailer && typeof globalThis.Bun === 'undefined') {
  const M = 2000
  const mailer = new email.Mailer({
    url: 'smtp://127.0.0.1:1',
    from: 'noreply@gradido.net',
    workers: 0,
    workerMax: 2,
    queueMax: M,
  })
  const t0 = process.hrtime.bigint()
  for (let i = 0; i < M; i++) mailer.send('member@example.org', TEMPLATE, LOCALE, values)
  const ns = Number(process.hrtime.bigint() - t0) / M
  mailer.close()
  console.log(`\n  sendTemplate() (render + queue, no JS string)`)
  console.log(`  ${''.padEnd(28)} ${ns.toFixed(0).padStart(7)} ns/mail   ${(1e9 / ns / 1e3).toFixed(0).padStart(6)} k/s`)
  console.log(`  vs pug render alone: ${(pugNs / ns).toFixed(1)}x faster, and that is before nodemailer`)
}
