/*
 * Does the preview show what ships?
 *
 * The page walks the op list in JavaScript the way ge_render_* walks it in C. If
 * those two ever disagree, the preview becomes a picture of a mail nobody sends --
 * which is the one failure that would make the whole tool worse than useless.
 *
 * So this does not reimplement the page's renderer, it EXTRACTS it out of
 * tools/preview-page.html and runs that. A change to the page is a change to what
 * is tested here; a copy would have been free to drift.
 *
 * The reference is tests/__snapshots_mjml__, filled with the same fixture values.
 */
import assert from 'node:assert/strict'
import fs from 'node:fs'
import path from 'node:path'
import { test } from 'node:test'
import { fileURLToPath } from 'node:url'
import { SNAPSHOT_DIR as SNAP } from '../tools/manifest.mjs'
import { pack } from '../tools/preview.mjs'
import { fixture } from '../tools/variants.mjs'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const IR = path.join(ROOT, 'gen', 'mjml', 'ir.json')

const ready = fs.existsSync(IR) && fs.existsSync(SNAP)

/** The page's own `render` and `esc`, taken out of the page. */
function pageRenderer() {
  const src = fs.readFileSync(path.join(ROOT, 'tools', 'preview-page.html'), 'utf8')
  const grab = (name) => {
    const at = src.indexOf(`function ${name}(`)
    const alt = at < 0 ? src.indexOf(`const ${name} = `) : at
    assert.ok(alt >= 0, `${name} not found in preview-page.html`)
    // to the blank line that ends the declaration
    const end = src.indexOf('\n\n', alt)
    return src.slice(alt, end < 0 ? undefined : end)
  }
  const factory = new Function(`${grab('esc')}\n${grab('render')}\nreturn render`)
  return factory()
}

test('the preview renders what the snapshots say', { skip: ready ? false : 'run: bun run snapshots:update' }, () => {
  const render = pageRenderer()
  const ir = JSON.parse(fs.readFileSync(IR, 'utf8'))

  let checked = 0
  for (const t of ir.templates) {
    const p = pack(t, ir.locales)
    const values = Object.fromEntries(p.slots.map((s) => [s, fixture(s)]))
    for (const kind of ['html', 'text', 'subject']) {
      p.docs[kind].forEach((perVariant, li) => {
        perVariant.forEach((doc, vi) => {
          const file = path.join(SNAP, t.name, `${ir.locales[li]}.${vi}.${kind}`)
          const want = fs.readFileSync(file, 'utf8')
          // `locale` is the renderer's, not a caller's -- the page passes the
          // locale it is showing, the same way C passes the one it was called with.
          const got = render(doc, p.pool, p.slots, values, ir.locales[li])
          assert.equal(got, want, `${t.name}/${ir.locales[li]}.${vi}.${kind}`)
          checked++
        })
      })
    }
  }
  assert.equal(checked, 810)
})
