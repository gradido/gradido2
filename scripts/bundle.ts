/**
 * `bun bundle` — the whole reference implementation as one executable.
 *
 * What comes out is a file that a community administrator can copy onto a server and start:
 * the backend, the frontends it hands out, the native addons, and bun itself. No runtime to
 * install, no `node_modules` beside it, nothing to serve the pages with. That is the promise
 * `Architecture.md` makes for a Gradido server, and this script is where it is kept for the
 * TypeScript path; `fast-servers/build.zig` keeps the same one for the C path, out of the same
 * `publish/` directory.
 *
 * Three steps, and the middle one is the only one with an idea in it:
 *
 * 1. **`bun run publish`.** Builds the frontends and assembles `publish/`, which is what both
 *    implementations serve. See `scripts/publish.ts`.
 * 2. **Write `packages/bundle/gen/main.ts`.** Bun embeds a file when a module imports it —
 *    `import x from './logo.png' with { type: 'file' }` yields a path inside the executable
 *    that `Bun.file()` opens like any other. A directory cannot be imported, so the entry
 *    point that names every file of every published site is generated rather than written.
 *    It is the only generated code here: the imports, a map from URL path to embedded file,
 *    and one call into `packages/bundle/src`.
 * 3. **`bun build --compile`** that entry.
 *
 * **The result is for the platform it was built on.** The embedded addons are compiled machine
 * code, so bun's cross-compilation targets are not offered here: a Linux build carries a Linux
 * `.node`, and there is no arrangement of flags that changes it.
 *
 *   bun bundle                       build/gradido, everything rebuilt as needed
 *   bun bundle --outfile=dist/srv    somewhere else
 *   bun bundle --skip-build          use what is already built (see the warning it prints)
 */
import { mkdir, writeFile } from 'node:fs/promises'
import { dirname, join, relative, resolve } from 'node:path'
import { type Manifest, PUBLISH, publish } from './publish'
import { run } from './run'

const ROOT = resolve(import.meta.dirname, '..')

/** Written by this script, imported by nothing else, gitignored. */
const ENTRY = 'packages/bundle/gen/main.ts'

async function main(): Promise<void> {
  const options = parseArguments(process.argv.slice(2))
  if (options.help) {
    console.log(usage())
    return
  }

  if (options.skipBuild) {
    console.warn(
      'bundle: --skip-build — the binary gets whatever is in the build directories now,\n' +
        '        including a frontend built for a different API_BASE_URL.',
    )
  }

  const manifest = await publish({ skipBuild: options.skipBuild })
  const entry = await writeEntry(manifest)
  await compile(entry, options.outfile)
}

type Options = {
  readonly outfile: string
  readonly skipBuild: boolean
  readonly help: boolean
}

function parseArguments(argv: readonly string[]): Options {
  let outfile = join('build', process.platform === 'win32' ? 'gradido.exe' : 'gradido')
  let skipBuild = false
  let help = false

  for (const argument of argv) {
    if (argument === '--help' || argument === '-h') {
      help = true
    } else if (argument === '--skip-build') {
      skipBuild = true
    } else if (argument.startsWith('--outfile=')) {
      outfile = argument.slice('--outfile='.length)
    } else {
      throw new Error(`unknown option "${argument}"\n\n${usage()}`)
    }
  }

  return { outfile: resolve(ROOT, outfile), skipBuild, help }
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

async function compile(entry: string, outfile: string): Promise<void> {
  await mkdir(dirname(outfile), { recursive: true })
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

  const size = Bun.file(outfile).size
  console.log(`bundle: ${relative(ROOT, outfile)} — ${(size / 1024 / 1024).toFixed(1)} MB`)
}

function usage(): string {
  return `bun bundle — build the single Gradido executable

  --outfile=<path>  where to write it (default: build/gradido)
  --skip-build      do not build the frontends and the addons first
  -h, --help        this text

The pages come from publish/, which \`bun run publish\` assembles and which the C build reads
as well. Set API_BASE_URL in the environment to point the pages at another origin.`
}

main().catch((error) => {
  console.error(error instanceof Error ? error.message : error)
  process.exit(1)
})
