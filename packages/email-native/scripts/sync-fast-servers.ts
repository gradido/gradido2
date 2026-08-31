/**
 * Puts the renderer into fast-servers, as C it compiles rather than C it generates.
 *
 * Four files: the two hand-written ones from this package and the two `build.zig` wrote
 * out of the pug templates. They land where every other service-core file lands — the
 * public headers under `include/service_core/`, the translation units in `src/` — so the
 * C build picks them up by walking those directories and needs no entry of its own.
 *
 * Why copy at all, rather than let `fast-servers/build.zig` run the codegen the way this
 * package's build does: the codegen is `node tools/gen_c.mjs`, and pug is a JS library.
 * `zig build` in fast-servers stays free of node, bun and node_modules, which is what
 * keeps that path buildable on its own.
 *
 * Run by the `build` script, straight after `c-cpp-zig-build`. A file whose bytes have
 * not changed is left alone, so a rebuild that changed nothing leaves no mtime and no
 * `git status` behind — and turbo's and zig's caches downstream stay warm.
 */

import { mkdir, readFile, stat, writeFile } from 'node:fs/promises'
import { dirname, join, relative, resolve } from 'node:path'

const ROOT = resolve(import.meta.dirname, '..')
const REPO = resolve(ROOT, '..', '..')

/** Overridable for a checkout elsewhere; the same option `build.zig` takes. */
const FAST_SERVERS = resolve(process.argv[2] ?? process.env.FAST_SERVERS ?? join(REPO, 'fast-servers'))

/** `build.zig`'s install prefix, which `c-cpp-zig-build` sets to `build`. */
const GEN = join(ROOT, 'build', 'gen')

const SERVICE_CORE = join(FAST_SERVERS, 'service-core')

const FILES = [
  { from: join(ROOT, 'include', 'service_core', 'email.h'), to: join(SERVICE_CORE, 'include', 'service_core', 'email.h') },
  { from: join(ROOT, 'src', 'email.c'), to: join(SERVICE_CORE, 'src', 'email.c') },
  { from: join(GEN, 'service_core', 'email_gen.h'), to: join(SERVICE_CORE, 'include', 'service_core', 'email_gen.h') },
  { from: join(GEN, 'email_gen.c'), to: join(SERVICE_CORE, 'src', 'email_gen.c') },
]

const exists = async (path: string) => {
  try {
    await stat(path)
    return true
  } catch {
    return false
  }
}

/**
 * The C path is droppable — `../../AGENTS.md`, "droppable, not merely removable" — so a
 * checkout without it is not an error here. It is also what makes this script safe to run
 * from a published package, where fast-servers is not shipped.
 */
if (!(await exists(SERVICE_CORE))) {
  console.log(`sync-fast-servers: ${relative(REPO, SERVICE_CORE)} is not there, nothing to copy`)
  process.exit(0)
}

let written = 0
for (const { from, to } of FILES) {
  if (!(await exists(from))) {
    console.error(
      `sync-fast-servers: ${relative(ROOT, from)} is missing.\n` +
        '  The generated half is written by the build; run `c-cpp-zig-build` first.',
    )
    process.exit(1)
  }

  const source = await readFile(from)
  if ((await exists(to)) && (await readFile(to)).equals(source)) continue

  await mkdir(dirname(to), { recursive: true })
  await writeFile(to, source)
  console.log(`sync-fast-servers: ${relative(REPO, to)}`)
  written++
}

console.log(
  written === 0
    ? 'sync-fast-servers: fast-servers is up to date'
    : `sync-fast-servers: ${written} file(s) copied into fast-servers/service-core`,
)
