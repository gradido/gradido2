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
 *   BUNDLE_C=0    the C binary. Off by default, because a first build fetches and
 *                 compiles h2o, LibreSSL and libpq, which takes minutes rather than
 *                 seconds. Nothing has to be installed for it: the Zig toolchain is
 *                 downloaded by c-cpp-zig-build, the same one shared-native uses
 *
 * Both take 0/1 or false/true. `BUNDLE_C=1 bun bundle` builds both; `BUNDLE_TS=0 BUNDLE_C=1`
 * builds only the C one. A third says how hard the C compiler tries:
 *
 *   BUNDLE_C_OPTIMIZE=fast   `fast`, `safe` or `small` — see C_OPTIMIZE below. The
 *                            default is a release mode, because a bundle is a release;
 *                            `zig build` on its own still gives a developer Debug
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
import { zigExe } from './zig'

const ROOT = resolve(import.meta.dirname, '..')

/** Written by this script, imported by nothing else, gitignored. */
const ENTRY = 'packages/bundle/gen/main.ts'

/**
 * How hard the C compiler tries, and at what.
 *
 * Zig's three release modes, under the names `c-cpp-zig-build` gives them — `shared-native`
 * declares `"optimize": "small"` and `email-native` `"fast"` in their `zigNative` blocks, and a
 * third spelling of the same three things would be one too many.
 *
 *   fast    ReleaseFast. What a deployment gets, and what the fast path is for
 *   safe    ReleaseSafe. The same optimiser, plus the checks that trap on undefined
 *           behaviour instead of continuing into it. Slower, and the one to ship when
 *           an installation matters more than a microsecond
 *   small   ReleaseSmall. Optimised for size, for a machine where that is the constraint
 *
 * Debug is deliberately not one of them: it is what `zig build` gives a developer and is not a
 * thing to hand to a community. `--version` reports which mode a binary carries, so a file on a
 * server can be asked rather than guessed about.
 */
const C_OPTIMIZE = {
  fast: 'ReleaseFast',
  safe: 'ReleaseSafe',
  small: 'ReleaseSmall',
} as const

type COptimize = keyof typeof C_OPTIMIZE

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
    await compileC(join(options.outdir, C_BINARY), options.cOptimize)
  }
}

type Options = {
  readonly outdir: string
  readonly typescript: boolean
  readonly c: boolean
  readonly cOptimize: COptimize
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

  return { outdir: resolve(ROOT, outdir), typescript, c, cOptimize: cOptimize(), help }
}

/** Which release mode the C binary is built in. Refused rather than guessed at, as above. */
function cOptimize(): COptimize {
  const value = process.env.BUNDLE_C_OPTIMIZE
  if (value === undefined || value === '') {
    return 'fast'
  }
  const mode = value.toLowerCase()
  if (!(mode in C_OPTIMIZE)) {
    throw new Error(`BUNDLE_C_OPTIMIZE="${value}" is none of ${Object.keys(C_OPTIMIZE).join(', ')}`)
  }
  return mode as COptimize
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
async function compileC(outfile: string, optimize: COptimize): Promise<void> {
  /* Not `zig` off the PATH: the toolchain is a dependency of this project rather than a
     prerequisite of the machine, so it is the one `c-cpp-zig-build` pins and downloads -- the
     same compiler that builds shared-native, on every machine the same. See scripts/zig.ts.
     Nothing has to be installed for a C binary but bun. */
  await run(
    [
      await zigExe(),
      'build',
      `-Doptimize=${C_OPTIMIZE[optimize]}`,
      '-p',
      dirname(outfile),
      '--prefix-exe-dir',
      '.',
    ],
    {},
    FAST_SERVERS,
  )
  report(outfile, C_OPTIMIZE[optimize])
}

function report(outfile: string, note = ''): void {
  /* Relative while it is inside the repository, absolute once `--outdir` points elsewhere: a
     line of `../../../..` is not a path anybody reads. */
  const shown = relative(ROOT, outfile)
  const where = shown.startsWith('..') ? outfile : shown
  const size = (Bun.file(outfile).size / 1024 / 1024).toFixed(1)
  console.log(`bundle: ${where} — ${size} MB${note === '' ? '' : `, ${note}`}`)
}

function usage(): string {
  return `bun bundle — build a Gradido server as one executable

  --outdir=<path>   where to write it (default: build)
  -h, --help        this text

  BUNDLE_TS=1       build build/${TS_BINARY}, the TypeScript path (the default)
  BUNDLE_C=0        build build/${C_BINARY}, the C path (downloads its own Zig
                    toolchain; a first build takes minutes)
  BUNDLE_C_OPTIMIZE=fast
                    how the C binary is optimised: fast, safe or small

A deployment runs one implementation or the other, never both against one database.

The pages come from publish/, which \`turbo publish\` assembles and which both binaries embed.
Set API_BASE_URL in the environment to point them at another origin.`
}

main().catch((error) => {
  console.error(error instanceof Error ? error.message : error)
  process.exit(1)
})
