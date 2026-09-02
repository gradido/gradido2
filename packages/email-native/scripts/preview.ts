/*
 * The preview, served and watched.
 *
 *   bun run preview          http://localhost:4321
 *   bun run preview --port 8080
 *
 * Three reasons this is a server and not a file:// page, and the third is the one
 * that decided it.
 *
 *   - fetch() from a file:// page to a sibling file is blocked (opaque origin), so
 *     the data would have to be embedded and every change would rebuild the page.
 *   - Without fetch there is no channel for a reload, so a changed template would
 *     mean pressing F5.
 *   - The images. A mail says src="includes/gradido-header.png"; a server maps that
 *     path and a file:// page needs the src rewritten to an absolute URL. Rewriting
 *     the mail to look at it is exactly the sort of workaround that later gets
 *     mistaken for the thing itself.
 *
 * `node tools/preview.mjs --inline` still writes standalone pages -- for handing
 * one to somebody or attaching it to a ticket, where embedding IS right.
 *
 * The watch is this file's own: bun's --hot reloads the modules THIS server
 * imports, and a .mjml is not one of them. So fs.watch over the sources, re-run
 * the two generators, and push one line down an SSE stream.
 */
import { watch } from 'node:fs'
import { mkdirSync } from 'node:fs'
import path from 'node:path'

const ROOT = path.resolve(import.meta.dir, '..')
const OUT = path.join(ROOT, 'gen', 'preview')
const argPort = process.argv.indexOf('--port')
const PORT = argPort >= 0 ? Number(process.argv[argPort + 1]) : 4321

const WATCHED = ['templates', 'po', '.preview-values.json', 'tools/manifest.mjs']

const clients = new Set<ReadableStreamDefaultController<Uint8Array>>()
const encoder = new TextEncoder()
const push = (line: string) => {
  for (const c of clients) {
    try {
      c.enqueue(encoder.encode(`data: ${line}\n\n`))
    } catch {
      clients.delete(c)
    }
  }
}

/** The two generators, in the order the second depends on the first. */
async function build(why: string): Promise<boolean> {
  const started = performance.now()
  for (const [tool, args] of [
    ['tools/extract_mjml.mjs', ['--out', 'gen/mjml']],
    ['tools/preview.mjs', []],
  ] as const) {
    const p = Bun.spawn(['node', tool, ...args], { cwd: ROOT, stdout: 'pipe', stderr: 'pipe' })
    const code = await p.exited
    if (code !== 0) {
      // A broken template is the normal state while editing one. Say what is
      // wrong and keep serving the last good build rather than going dark.
      const err = await new Response(p.stderr).text()
      console.error(`\n✗ ${why}\n${err.trim()}\n`)
      return false
    }
  }
  console.log(`✓ ${why} (${(performance.now() - started).toFixed(0)} ms)`)
  return true
}

mkdirSync(OUT, { recursive: true })
if (!(await build('erster Build'))) process.exit(1)

let pending: ReturnType<typeof setTimeout> | null = null
for (const target of WATCHED) {
  watch(path.join(ROOT, target), { recursive: true }, (_event, file) => {
    // An editor writes a file in several steps; one rebuild per burst is enough.
    if (pending) clearTimeout(pending)
    pending = setTimeout(async () => {
      pending = null
      if (await build(String(file))) push(String(file))
    }, 120)
  })
}

const file = (p: string) => Bun.file(path.join(ROOT, p))

const server = Bun.serve({
  port: PORT,
  async fetch(req) {
    const { pathname } = new URL(req.url)

    if (pathname === '/events') {
      return new Response(
        new ReadableStream<Uint8Array>({
          start(c) {
            clients.add(c)
            c.enqueue(encoder.encode(': verbunden\n\n'))
          },
          cancel(c) {
            clients.delete(c as never)
          },
        }),
        { headers: { 'content-type': 'text/event-stream', 'cache-control': 'no-cache' } },
      )
    }

    // The images a mail references, at the path the mail references them by.
    if (pathname.startsWith('/includes/')) {
      const f = file(path.join('templates', pathname))
      if (await f.exists()) return new Response(f)
    }

    const name = pathname === '/' ? '/index.html' : pathname
    const f = file(path.join('gen', 'preview', name))
    if (await f.exists()) return new Response(f, { headers: { 'cache-control': 'no-store' } })
    return new Response('not found', { status: 404 })
  },
})

console.log(`\n  http://localhost:${server.port}`)
console.log(`  beobachtet: ${WATCHED.join(', ')}\n`)
