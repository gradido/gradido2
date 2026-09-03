/**
 * `bun bundle` — a Gradido server as one executable, from either implementation.
 *
 * What comes out is a file a community administrator can copy onto a server and start: the
 * server, the frontends it hands out, and its runtime. No runtime to install, no
 * `node_modules` beside it, nothing to serve the pages with. That is the promise
 * `Architecture.md` makes, and this script is where it is kept.
 *
 *   build/gradido2        the TypeScript path — the reference implementation
 *   build/gradido2-fast   the C path, out of fast-servers/
 *
 * **Which of them is built is a decision, and the default is the reference one.** They are two
 * implementations of the same server and a deployment runs one or the other, never both against
 * one database — `AGENTS.md`, *C is the fast implementation*. Two environment variables say
 * which to build, so a release job sets them once rather than remembering two commands:
 *
 *   BUNDLE_TS=1   the TypeScript binary. The default
 *   BUNDLE_C=0    the C binary. Off by default, because it needs a zig toolchain and
 *                 a first build fetches and compiles h2o, LibreSSL and libpq
 *
 * Both take 0/1 or false/true. `BUNDLE_C=1 bun bundle` builds both; `BUNDLE_TS=0 BUNDLE_C=1`
 * builds only the C one.
 *
 * The TypeScript binary is built in three steps, and the middle one is the only one with an
 * idea in it:
 *
 * 1. **`turbo publish`.** Builds the frontends and assembles `publish/`, which is what both
 *    implementations serve. See `scripts/publish.ts`.
 * 2. **Write `packages/bundle/gen/main.ts`.** Bun embeds a file when a module imports it —
 *    `import x from './logo.png' with { type: 'file' }` yields a path inside the executable
 *    that `Bun.file()` opens like any other. A directory cannot be imported, so the entry
 *    point that names every file of every published site is generated rather than written.
 * 3. **`bun build --compile`** that entry.
 *
 * The C binary is `zig build`, which does the same three things in its own way. It publishes
 * again too; `publish` is a turbo task, so the second ask is a cache hit and neither build has
 * to know which of them ran first.
 *
 * **The TypeScript binary is for the platform it was built on.** The embedded addons are
 * compiled machine code, so bun's cross-compilation targets are not offered here. The C one
 * cross-compiles freely; `fast-servers/README.md` has the targets.
 *
 *   bun bundle                       build/, everything rebuilt as needed
 *   BUNDLE_C=1 bun bundle            both implementations
 *   bun bundle --outdir=dist         somewhere else
 */
import { mkdir, writeFile } from 'node:fs/promises'
import { dirname, join, relative, resolve } from 'node:path'
import { type Manifest, PUBLISH, readManifest } from './publish'
import { run, turbo } from './run'

const ROOT = resolve(import.meta.dirname, '..')

/** Written by this script, imported by nothing else, gitignored. */
const ENTRY = 'packages/bundle/gen/main.ts'

/** The product's name. The C implementation is the same product, and says which one it is. */
const TS_BINARY = process.platform === 'win32' ? 'gradido2.exe' : 'gradido2'
const C_BINARY = process.platform === 'win32' ? 'gradido2-fast.exe' : 'gradido2-fast'

const FAST_SERVERS = join(ROOT, 'fast-servers')

async function main(): Promise<void> {
  const options = parseArguments(process.argv.slice(2))
  if (options.help) {
    console.log(usage())
    return
  }

  /* The pages come first and are built once, whichever implementations follow: both embed the
     same publish/, which is the point of it existing.

     Through turbo rather than by calling publish() here, because the C build asks for the same
     thing a moment later and turbo is what knows whether it still has to happen. A cache hit
     there costs milliseconds; a second answer to that question written into this script would
     be a worse copy of turbo's. */
  await turbo(['publish'], {})
  const manifest = await readManifest()
  await mkdir(options.outdir, { recursive: true })

  if (options.typescript) {
    const entry = await writeEntry(manifest)
    await compileTypeScript(entry, join(options.outdir, TS_BINARY))
  }
  if (options.c) {
    await compileC(join(options.outdir, C_BINARY))
  }
}

type Options = {
  readonly outdir: string
  readonly typescript: boolean
  readonly c: boolean
  readonly help: boolean
}

function parseArguments(argv: readonly string[]): Options {
  let outdir = 'build'
  let help = false

  for (const argument of argv) {
    if (argument === '--help' || argument === '-h') {
      help = true
    } else if (argument.startsWith('--outdir=')) {
      outdir = argument.slice('--outdir='.length)
    } else {
      throw new Error(`unknown option "${argument}"\n\n${usage()}`)
    }
  }

  const typescript = flag('BUNDLE_TS', true)
  const c = flag('BUNDLE_C', false)
  if (!help && !typescript && !c) {
    throw new Error('BUNDLE_TS=0 and BUNDLE_C=0: there is nothing to build')
  }

  return { outdir: resolve(ROOT, outdir), typescript, c, help }
}

/**
 * One of the two switches, read from the environment.
 *
 * A value that is neither is refused rather than guessed at: `BUNDLE_C=yes` meaning "no" is a
 * release built without the binary somebody asked for, and the only place that could be
 * noticed is a directory listing nobody reads.
 */
function flag(name: string, fallback: boolean): boolean {
  const value = process.env[name]
  if (value === undefined || value === '') {
    return fallback
  }
  if (['0', 'false', 'no'].includes(value.toLowerCase())) {
    return false
  }
  if (['1', 'true', 'yes'].includes(value.toLowerCase())) {
    return true
  }
  throw new Error(`${name}="${value}" is neither 0 nor 1`)
}

/**
 * Writes the entry point: every embedded file, and the call that starts the binary.
 *
 * Generated and not written by hand for the reason in this file's header — a directory has no
 * import — and generated *into the package* rather than into a temporary directory, because
 * `../src` and `@gradido/backend` have to resolve, and that is what a workspace package's
 * `node_modules` is for.
 *
 * What it embeds is `publish/sites.json`, not a directory walk: the manifest is what the C
 * build reads too, so the two binaries cannot end up carrying different sets of files.
 */
async function writeEntry(manifest: Manifest): Promise<string> {
  const version = (await Bun.file(join(ROOT, 'package.json')).json()).version as string
  const imports: string[] = []
  const sites: string[] = []

  for (const [siteIndex, site] of manifest.sites.entries()) {
    const identifier = (fileIndex: number) => `s${siteIndex}f${fileIndex}`
    const entries: string[] = []

    for (const [fileIndex, file] of site.files.entries()) {
      /* Relative to the generated file, which sits in packages/bundle/gen. */
      const from = relative(dirname(join(ROOT, ENTRY)), join(PUBLISH, site.dir, file.path))
      imports.push(
        `import ${identifier(fileIndex)} from ${JSON.stringify(from.split(/[\\/]/u).join('/'))} with { type: 'file' }`,
      )
      /* The type and the ETag travel from the manifest into the binary, unchanged — see
         StaticFile in packages/backend/src/server/staticRoutes.ts for why they are not worked
         out at runtime. */
      entries.push(
        `        [${JSON.stringify(file.path)}, { file: ${identifier(fileIndex)},` +
          ` type: ${JSON.stringify(file.type)}, etag: ${JSON.stringify(file.etag)},` +
          ` immutable: ${file.immutable} }],`,
      )
    }

    const indexAt = site.files.findIndex((file) => file.path === site.index)

    sites.push(
      [
        '    {',
        `      name: ${JSON.stringify(site.name)},`,
        `      basePath: ${JSON.stringify(site.basePath)},`,
        `      index: { file: ${identifier(indexAt)},` +
          ` type: ${JSON.stringify(site.files[indexAt]?.type)},` +
          ` etag: ${JSON.stringify(site.files[indexAt]?.etag)}, immutable: false },`,
        '      files: new Map([',
        ...entries,
        '      ]),',
        '    },',
      ].join('\n'),
    )
  }

  const source = [
    '/* Generated by scripts/bundle.ts out of publish/. Not edited, not committed —',
    '   see packages/bundle/.gitignore.',
    '',
    '   Every import below is a file bun copies into the executable; what it binds is the path',
    '   that file has inside it. The map turns a URL path back into one of them. */',
    "import { startGradido } from '../src'",
    ...imports,
    '',
    'startGradido(process.argv.slice(2), {',
    `  version: ${JSON.stringify(version)},`,
    '  sites: [',
    ...sites,
    '  ],',
    '})',
    '',
  ].join('\n')

  const path = join(ROOT, ENTRY)
  await mkdir(dirname(path), { recursive: true })
  await writeFile(path, source)
  return path
}

async function compileTypeScript(entry: string, outfile: string): Promise<void> {
  /* --target=bun and nothing else: the embedded addons are native code for this platform, so
     bun's cross-compilation targets would produce a binary that cannot load its own contents. */
  await run(
    [
      process.execPath,
      'build',
      '--compile',
      '--target=bun',
      `--outfile=${outfile}`,
      relative(ROOT, entry),
    ],
    {},
  )
  report(outfile)
}

/**
 * The C implementation, into the same directory.
 *
 * `zig build` installs into `fast-servers/zig-out` by default, which is where a plain
 * `zig build` should keep putting it — that is the C build's own convention and its README's.
 * A *release* is this script's business, so the prefix is pointed at `build/` and the `bin/`
 * below it is flattened away: one directory with the two binaries somebody deploys.
 *
 * It publishes again, and that is deliberate: `publish` is a turbo task, so the second ask is a
 * cache hit rather than a second build. Which of the two callers ran first is then not something
 * either of them has to know.
 */
async function compileC(outfile: string): Promise<void> {
  if (Bun.which('zig') === null) {
    /* Asked for by an environment variable, so the failure has to name it: "zig: not found" a
       minute into a release job says nothing about which switch wanted it. */
    throw new Error(
      'BUNDLE_C=1 needs a zig toolchain on the PATH, which is what builds the C server.\n' +
        '  fast-servers/README.md has what it does with it. Leave BUNDLE_C unset to build\n' +
        '  only the TypeScript binary.',
    )
  }
  await run(['zig', 'build', '-p', dirname(outfile), '--prefix-exe-dir', '.'], {}, FAST_SERVERS)
  report(outfile)
}

function report(outfile: string): void {
  console.log(
    `bundle: ${relative(ROOT, outfile)} — ${(Bun.file(outfile).size / 1024 / 1024).toFixed(1)} MB`,
  )
}

function usage(): string {
  return `bun bundle — build a Gradido server as one executable

  --outdir=<path>   where to write it (default: build)
  -h, --help        this text

  BUNDLE_TS=1       build build/${TS_BINARY}, the TypeScript path (the default)
  BUNDLE_C=0        build build/${C_BINARY}, the C path (needs a zig toolchain)

A deployment runs one implementation or the other, never both against one database.

The pages come from publish/, which \`turbo publish\` assembles and which both binaries embed.
Set API_BASE_URL in the environment to point them at another origin.`
}

main().catch((error) => {
  console.error(error instanceof Error ? error.message : error)
  process.exit(1)
})
