// Writes tests/__snapshots__ from the pug sources. The updater, and the only thing
// that writes there -- everything else compares.
//
//   bun run snapshots:update      after a template, a locale or a pug change
//
// Then read the diff: it is the whole point of the snapshots. A pug upgrade that
// changes whitespace shows up here as 540 changed files, an edited sentence as one.

import fs from 'fs'
import path from 'path'
import { SNAPSHOT_DIR } from './manifest.mjs'
import { renderAll } from './render_pug.mjs'

/** Every file under `dir`, relative to it. */
function walk(dir, prefix = '') {
  const out = []
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const rel = path.join(prefix, entry.name)
    if (entry.isDirectory()) out.push(...walk(path.join(dir, entry.name), rel))
    else out.push(rel)
  }
  return out
}

const before = fs.existsSync(SNAPSHOT_DIR) ? new Set(walk(SNAPSHOT_DIR)) : new Set()
const written = new Set()
let changed = 0

for (const doc of renderAll()) {
  const file = path.join(SNAPSHOT_DIR, doc.snapshot)
  const text = Buffer.from(doc.text, 'utf8')
  written.add(doc.snapshot)

  // An unchanged file is left alone, so `git status` after an update names exactly
  // the documents a change reached.
  if (before.has(doc.snapshot) && fs.readFileSync(file).equals(text)) continue

  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, text)
  changed++
}

let removed = 0
for (const stale of before) {
  if (written.has(stale)) continue
  fs.rmSync(path.join(SNAPSHOT_DIR, stale))
  removed++
}
// Leave no empty template directory behind when a template is dropped.
if (fs.existsSync(SNAPSHOT_DIR))
  for (const entry of fs.readdirSync(SNAPSHOT_DIR, { withFileTypes: true }))
    if (entry.isDirectory() && fs.readdirSync(path.join(SNAPSHOT_DIR, entry.name)).length === 0)
      fs.rmdirSync(path.join(SNAPSHOT_DIR, entry.name))

console.error(
  `${written.size} documents rendered from pug: ${changed} written, ` +
    `${written.size - changed} unchanged, ${removed} removed.`,
)
console.error(`  ${SNAPSHOT_DIR}`)
