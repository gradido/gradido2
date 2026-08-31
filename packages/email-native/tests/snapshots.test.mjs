/*
 * The pug half of the snapshot guarantee: what pug renders today has to equal what
 * is checked in under tests/__snapshots__.
 *
 * The pug sources are the source of truth; the snapshots are how a change to them
 * becomes visible. A new pug version, an edited template, a corrected translation --
 * each shows up here as a diff over exactly the documents it reached, and nowhere
 * else. `bun run snapshots:update` is what accepts such a change.
 *
 * The other half is elsewhere and reads the same files: tests/addon.test.js compares
 * the addon against them, and `zig build check` compares the whole C matrix
 * (tools/verify.mjs). So neither implementation is ever compared against itself.
 */

import assert from 'node:assert'
import fs from 'node:fs'
import path from 'node:path'
import test from 'node:test'
import { SNAPSHOT_DIR } from '../tools/manifest.mjs'
import { renderAll } from '../tools/render_pug.mjs'

const HINT = 'run `bun run snapshots:update` and read the diff'

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

test('pug renders what the snapshots say', () => {
  const onDisk = new Set(walk(SNAPSHOT_DIR))
  const rendered = new Set()
  const differ = []

  for (const doc of renderAll()) {
    rendered.add(doc.snapshot)
    const file = path.join(SNAPSHOT_DIR, doc.snapshot)
    if (!onDisk.has(doc.snapshot)) {
      differ.push(`${doc.snapshot}: no snapshot for it`)
      continue
    }
    if (!fs.readFileSync(file).equals(Buffer.from(doc.text, 'utf8')))
      differ.push(`${doc.snapshot}: pug renders something else now`)
  }

  for (const stale of onDisk)
    if (!rendered.has(stale)) differ.push(`${stale}: snapshot for a document nobody renders`)

  assert.deepEqual(differ, [], `${differ.length} document(s) differ -- ${HINT}`)
  // 17 templates x 10 locales x variants x {html, subject}. A template that stopped
  // being rendered at all would otherwise pass the loop above in silence.
  assert.equal(rendered.size, 540)
})
