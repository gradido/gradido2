/*
 * Writes tests/__snapshots_mjml__ out of gen/mjml/ir.json -- the MJML path's half
 * of what tests/__snapshots__ is for the pug one.
 *
 *   bun run extract:mjml && node tools/snapshots_mjml.mjs
 *
 * Same idea, same value: the snapshots are how a change becomes VISIBLE. An edited
 * template, a corrected translation or an MJML upgrade shows up as a diff over
 * exactly the documents it reached and over nothing else. Read that diff -- it is
 * the review.
 *
 * It renders nothing. The IR already holds every document as [literal, slot, ...];
 * filling the slots with the fixture values is what ge_render_* does at runtime,
 * so what lands on disk is a finished mail and not an intermediate form. That is
 * also why this file is 60 lines and tools/snapshots.mjs needs a pug renderer.
 *
 * Two snapshot trees exist on purpose while both engines do: __snapshots__ is
 * pug's, __snapshots_mjml__ is this one, and tools/compare_pug.mjs is what holds
 * them to each other. When pug goes, this becomes the only one.
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { fixture } from './variants.mjs'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const arg = (name, fallback) => {
  const i = process.argv.indexOf(`--${name}`)
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback
}
const IR = path.resolve(arg('ir', path.join(ROOT, 'gen', 'mjml', 'ir.json')))
const OUT = path.resolve(arg('out', path.join(ROOT, 'tests', '__snapshots_mjml__')))

const escape = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c])

/** ge_render_*: literals as they are, slot values escaped by the op's mode. */
const fill = (ops) =>
  ops
    .map((op) => (op.t === 'lit' ? op.s : op.esc === 'html' ? escape(fixture(op.name)) : fixture(op.name)))
    .join('')

const walk = (dir, prefix = '') =>
  fs.existsSync(dir)
    ? fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) =>
        e.isDirectory() ? walk(path.join(dir, e.name), path.join(prefix, e.name)) : [path.join(prefix, e.name)],
      )
    : []

if (!fs.existsSync(IR)) {
  console.error(`no ${path.relative(ROOT, IR)} -- run: bun run extract:mjml`)
  process.exit(1)
}
const ir = JSON.parse(fs.readFileSync(IR, 'utf8'))

const before = new Set(walk(OUT))
const written = new Set()
let changed = 0

for (const t of ir.templates) {
  for (const [kind, ext] of [['html', 'html'], ['subject', 'subject'], ['text', 'text']]) {
    t.renders[kind].forEach((perVariant, li) => {
      perVariant.forEach((ops, vi) => {
        const rel = path.join(t.name, `${ir.locales[li]}.${vi}.${ext}`)
        const body = Buffer.from(`${fill(ops)}\n`, 'utf8')
        written.add(rel)
        const file = path.join(OUT, rel)
        // An unchanged file is left alone, so `git status` after an update names
        // exactly the documents a change reached.
        if (before.has(rel) && fs.readFileSync(file).equals(body)) return
        fs.mkdirSync(path.dirname(file), { recursive: true })
        fs.writeFileSync(file, body)
        changed++
      })
    })
  }
}

let removed = 0
for (const stale of before) {
  if (written.has(stale)) continue
  fs.rmSync(path.join(OUT, stale))
  removed++
}

console.log(
  `${written.size} documents in ${path.relative(ROOT, OUT)}` +
    ` -- ${changed} written, ${removed} removed, ${written.size - changed} unchanged`,
)
if (changed || removed) console.log('now read `git diff` over them: that is the review.')
