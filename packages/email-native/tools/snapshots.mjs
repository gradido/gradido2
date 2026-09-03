/*
 * Writes tests/__snapshots__ out of gen/mjml/ir.json.
 *
 *   bun run snapshots:update
 *
 * Same idea, same value: the snapshots are how a change becomes VISIBLE. An edited
 * template, a corrected translation or an MJML upgrade shows up as a diff over
 * exactly the documents it reached and over nothing else. Read that diff -- it is
 * the review.
 *
 * It renders nothing. The IR already holds every document as [literal, slot, ...];
 * filling the slots with the fixture values is what ge_render_* does at runtime,
 * so what lands on disk is a finished mail and not an intermediate form. That is
 * also why this file is 60 lines rather than needing a renderer of its own.
 *
 * These are what `zig build check` holds the C binary to, and what
 * tests/snapshots.test.mjs holds the extractor to. Nothing compares an
 * implementation against itself: the snapshots come from the JS side, the C is
 * measured against them.
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { SNAPSHOT_DIR } from './manifest.mjs'
import { fixture } from './variants.mjs'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const arg = (name, fallback) => {
  const i = process.argv.indexOf(`--${name}`)
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback
}
const IR = path.resolve(arg('ir', path.join(ROOT, 'gen', 'mjml', 'ir.json')))
const OUT = path.resolve(arg('out', SNAPSHOT_DIR))

const escape = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c])

/*
 * ge_render_*: literals as they are, slot values escaped by the op's mode.
 *
 * `locale` is the exception and not an oversight -- it is filled by the RENDERER
 * from the locale it was called with, never by a caller, so a fixture value there
 * would describe a document that cannot exist.
 */
const fill = (ops, locale) =>
  ops
    .map((op) => {
      if (op.t === 'lit') return op.s
      const value = op.name === 'locale' ? locale : fixture(op.name)
      return op.esc === 'html' ? escape(value) : value
    })
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
        // No trailing newline: tools/dump.c writes the document and nothing else,
        // and tools/verify.mjs compares byte for byte.
        const body = Buffer.from(fill(ops, ir.locales[li]), 'utf8')
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
