// Compares what the C binary rendered against the committed snapshots, byte for
// byte. `zig build check` writes the C side with tools/dump.c and runs this over it;
// nothing here touches pug, which is why the check does not need the templates.
//
// The snapshots are the pug output (tools/snapshots.mjs wrote them) and
// tests/snapshots.test.mjs is what keeps them that. So this comparison is the second
// half of "the C renders what pug renders", and a failure here means the generated
// tables and the templates have come apart -- rebuild, and if the templates really
// changed, update the snapshots and read that diff first.

import fs from 'fs'
import path from 'path'
import { SNAPSHOT_DIR } from './manifest.mjs'

const arg = (name, fallback) => {
  const i = process.argv.indexOf(`--${name}`)
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback
}
const C_DIR = path.resolve(arg('c', 'out/c')) // written by tools/dump.c

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

const snapshots = walk(SNAPSHOT_DIR).sort()
if (snapshots.length === 0) {
  console.error(`no snapshots in ${SNAPSHOT_DIR} -- run \`bun run snapshots:update\``)
  process.exit(1)
}

const diffs = []
const seen = new Set()
let checked = 0

for (const rel of snapshots) {
  // tests/__snapshots__/<template>/<locale>.<combo>.<kind>  ->  dump's flat name
  const [template, file] = rel.split(path.sep)
  const name = `${template}.${file}`
  seen.add(name)

  const cPath = path.join(C_DIR, name)
  if (!fs.existsSync(cPath)) {
    diffs.push(`${name}: not produced by the C binary`)
    continue
  }
  checked++
  if (!fs.readFileSync(cPath).equals(fs.readFileSync(path.join(SNAPSHOT_DIR, rel))))
    diffs.push(`${name}: differs from the snapshot`)
}

for (const name of fs.readdirSync(C_DIR))
  if (!seen.has(name)) diffs.push(`${name}: rendered by the C binary, but no snapshot for it`)

if (diffs.length) {
  console.error(`${diffs.length} mismatch(es) against ${SNAPSHOT_DIR}:`)
  for (const d of diffs.slice(0, 20)) console.error(`  ${d}`)
  if (diffs.length > 20) console.error(`  ... and ${diffs.length - 20} more`)
  console.error(
    '\nEither the generated tables are stale (rebuild), or the templates changed and\n' +
      'the snapshots were not updated (`bun run snapshots:update`, then read the diff).',
  )
  process.exit(1)
}
console.error(`OK: ${checked} documents from the C binary == the snapshots, byte for byte.`)
