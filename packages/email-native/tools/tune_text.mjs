/*
 * A bench for TEXT_OPTIONS, and nothing else.
 *
 * The plain text part is html-to-text over the rendered HTML, so tuning it means
 * trying selectors and looking at 270 documents. Re-running the extractor for each
 * attempt would mean 270 MJML compiles per attempt; instead the sentinel HTML is
 * rebuilt straight out of gen/mjml/ir.json -- it is exactly the string html-to-text
 * was handed -- and only the conversion is repeated.
 *
 *   node tools/tune_text.mjs              score the options in manifest.mjs
 *   node tools/tune_text.mjs --show NAME  print pug next to mjml for one template
 *
 * The score to drive to 270 is the first line. It is a means, not the goal: where
 * the pug text was worse, deviating from it deliberately is the right answer, and
 * --show is how that is judged.
 */
import { convert } from 'html-to-text'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { TEXT_OPTIONS_MJML as TEXT_OPTIONS } from './manifest.mjs'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const arg = (name, fallback) => {
  const i = process.argv.indexOf(`--${name}`)
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback
}
const SHOW = arg('show', null)

const SOT = String.fromCharCode(1)
const EOT = String.fromCharCode(2)
const SPLIT_RE = new RegExp(`${SOT}([^${SOT}${EOT}]*)${EOT}`)
const sentinel = (n) => SOT + n + EOT

const unescapeHtml = (s) =>
  s.replace(/&(amp|lt|gt|quot|#39|#x27);/g, (m, e) =>
    ({ amp: '&', lt: '<', gt: '>', quot: '"', '#39': "'", '#x27': "'" })[e],
  )

/** Back to the exact string the extractor handed html-to-text. */
const toSentinelHtml = (ops) => ops.map((o) => (o.t === 'lit' ? o.s : sentinel(o.name))).join('')

/** The literals of a text op list, joined -- the shape to compare. */
const textOf = (ops) => ops.map((o) => (o.t === 'lit' ? o.s : sentinel(o.name))).join('')

const chunkText = (text) => {
  const parts = text.split(SPLIT_RE)
  const ops = []
  for (let i = 0; i < parts.length; i++) {
    if (i % 2 === 0) { if (parts[i]) ops.push({ t: 'lit', s: unescapeHtml(parts[i]) }) }
    else ops.push({ t: 'slot', name: parts[i], esc: 'raw' })
  }
  return ops
}

const read = (d) => JSON.parse(fs.readFileSync(path.join(ROOT, 'gen', d, 'ir.json'), 'utf8'))
const pug = read('pug')
const mjml = read('mjml')
const P = Object.fromEntries(pug.templates.map((t) => [t.name, t]))
const M = Object.fromEntries(mjml.templates.map((t) => [t.name, t]))

let same = 0
let total = 0
const offenders = new Map()

for (const name of Object.keys(P)) {
  for (let li = 0; li < P[name].renders.text.length; li++) {
    for (let vi = 0; vi < P[name].renders.text[li].length; vi++) {
      const want = textOf(P[name].renders.text[li][vi]).trim()
      const html = toSentinelHtml(M[name].renders.html[li][vi])
      const got = textOf(chunkText(convert(html, TEXT_OPTIONS).trim()))
      total++
      if (want === got) same++
      else offenders.set(name, (offenders.get(name) ?? 0) + 1)

      if (SHOW === name && li === 0 && vi === 0) {
        const side = (a, b) => {
          const A = a.split('\n')
          const B = b.split('\n')
          const w = Math.max(...A.map((l) => l.length), 20)
          console.log(`  ${'pug'.padEnd(Math.min(w, 60))} | mjml`)
          console.log(`  ${'-'.repeat(Math.min(w, 60))}-+-${'-'.repeat(20)}`)
          for (let i = 0; i < Math.max(A.length, B.length); i++) {
            const l = (A[i] ?? '').slice(0, 60)
            const r = (B[i] ?? '').slice(0, 60)
            console.log(`  ${l.padEnd(Math.min(w, 60))} ${l === r ? '|' : '≠'} ${r}`)
          }
        }
        side(want, got)
        console.log()
      }
    }
  }
}

console.log(`  exakt gleich ${same}/${total}`)
if (offenders.size) {
  console.log('\n  noch abweichend:')
  for (const [n, c] of [...offenders].sort((a, b) => b[1] - a[1]))
    console.log(`    ${n.padEnd(32)} ${c}`)
  console.log('\n  node tools/tune_text.mjs --show <name>   zeigt pug neben mjml')
}
