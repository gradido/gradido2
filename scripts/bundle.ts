/**
 * `bun bundle` — the whole reference implementation as one executable.
 *
 * What comes out is a file that a community administrator can copy onto a server and start:
 * the backend, the frontends it hands out, the native addons, and bun itself. No runtime to
 * install, no `node_modules` beside it, nothing to serve the pages with. That is the promise
 * `Architecture.md` makes for a Gradido server, and this script is where it is kept.
 *
 * Three steps, and the middle one is the only one with an idea in it:
 *
 * 1. **Build what goes in.** `turbo build` per frontend and once for the native addons, so
 *    the graph and the cache are turbo's problem rather than this script's.
 * 2. **Write `packages/bundle/gen/main.ts`.** Bun embeds a file when a module imports it —
 *    `import x from './logo.png' with { type: 'file' }` yields a path inside the executable
 *    that `Bun.file()` opens like any other. A directory cannot be imported, so the entry
 *    point that names every file of every built frontend is generated rather than written.
 *    It is the only generated code here: it holds the imports, a map from URL path to
 *    embedded file, and one call into `packages/bundle/src`.
 * 3. **`bun build --compile`** that entry.
 *
 * The frontends are built with `API_BASE_URL` empty and `BASE_PATH` set to where the binary
 * mounts them, because in a bundle the server that hands out a page is the server the page
 * calls — see `packages/frontend/src/config/schema.ts`. Pass `API_BASE_URL=…` to override
 * that for a deployment that puts the API somewhere else.
 *
 * **The result is for the platform it was built on.** The embedded addons are compiled
 * machine code, so bun's cross-compilation targets are not offered here: a Linux build
 * carries a Linux `.node`, and there is no arrangement of flags that changes it.
 *
 *   bun bundle                       build/gradido, everything rebuilt as needed
 *   bun bundle --outfile=dist/srv    somewhere else
 *   bun bundle --skip-build          use what is already built (see the warning it prints)
 */
import { existsSync } from 'node:fs'
import { mkdir, writeFile } from 'node:fs/promises'
import { dirname, join, relative, resolve } from 'node:path'

const ROOT = resolve(import.meta.dirname, '..')

/**
 * The frontends the binary carries, in the order `staticRoutes` should match them.
 *
 * `basePath` is both where the site is mounted and what it is built with: vite writes it into
 * every asset URL of `index.html`, so the two cannot be decided separately.
 */
const SITES = [
  {
    name: 'frontend',
    package: '@gradido/frontend',
    basePath: '',
    build: 'packages/frontend/build',
  },
  /* `packages/admin` does not exist yet. It is built exactly like the frontend — vite,
     the same `frontend-core`, its own `build/` — so it joins this list rather than needing
     anything new:

       { name: 'admin', package: '@gradido/admin', basePath: '/admin',
         build: 'packages/admin/build' },

     `staticRoutes` already prefers the longer `basePath`, so `/admin/…` goes to the admin
     app and everything else to the frontend, and vite has to be told `BASE_PATH=/admin` —
     which is what building it from this entry does. */
] as const

/**
 * Packages whose build output is native code the binary needs.
 *
 * `shared-native` arrives on its own — the frontend build depends on it through
 * `@gradido/shared` — but naming it here is what makes the *backend's* copy in the executable
 * a thing this script arranged rather than a side effect of something else's dependency.
 * `email-native` has no importer yet at all; `packages/bundle/src/natives.ts` says why it is
 * in the binary anyway.
 */
const NATIVE_PACKAGES = ['@gradido/shared-native', '@gradido/email-native'] as const

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
  } else {
    await buildInputs()
  }

  const entry = await writeEntry()
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
 * Everything that has to exist before there is anything to embed.
 *
 * One `turbo build` per site, because each is built with its own `BASE_PATH` and turbo takes
 * one environment per run. The native packages go in a single run afterwards; `^build` has
 * already brought `shared-native` along by then, and turbo answers from its cache rather than
 * building it twice.
 */
async function buildInputs(): Promise<void> {
  for (const site of SITES) {
    await turbo(['build', `--filter=${site.package}`], {
      /* Empty unless a deployment says otherwise: the page is served by the server it calls.
         `process.env` wins over the `.env` that vite loads, so a development `.env` in the
         package cannot leak a localhost URL into a binary. */
      API_BASE_URL: process.env.API_BASE_URL ?? '',
      BASE_PATH: site.basePath,
    })
  }
  await turbo(['build', ...NATIVE_PACKAGES.map((name) => `--filter=${name}`)], {})

  /* Nothing above is allowed to have quietly produced nothing. A missing directory here means
     a build task that did not run, and the alternative to saying so is a binary with no pages
     in it that only fails when somebody opens it in a browser. */
  for (const site of SITES) {
    if (!existsSync(join(ROOT, site.build))) {
      throw new Error(`${site.package} produced no ${site.build}`)
    }
  }
}

/** The files of one site, relative to its build directory, in a stable order. */
async function siteFiles(build: string): Promise<string[]> {
  const glob = new Bun.Glob('**/*')
  const files: string[] = []
  for await (const file of glob.scan({ cwd: join(ROOT, build), onlyFiles: true })) {
    /* Bun.Glob yields the platform separator on Windows; a URL path uses the other one. */
    files.push(file.split(/[\\/]/u).join('/'))
  }
  return files.sort()
}

/**
 * Writes the entry point: every embedded file, and the call that starts the binary.
 *
 * Generated and not written by hand for the reason in this file's header — a directory has no
 * import — and generated *into the package* rather than into a temporary directory, because
 * `../src` and `@gradido/backend` have to resolve, and that is what a workspace package's
 * `node_modules` is for.
 */
async function writeEntry(): Promise<string> {
  const version = (await Bun.file(join(ROOT, 'package.json')).json()).version as string
  const imports: string[] = []
  const sites: string[] = []

  for (const [siteIndex, site] of SITES.entries()) {
    const files = await siteFiles(site.build)
    if (!files.includes('index.html')) {
      throw new Error(`${site.build}/index.html is missing — ${site.package} built no app`)
    }

    const identifier = (fileIndex: number) => `s${siteIndex}f${fileIndex}`
    const entries: string[] = []

    for (const [fileIndex, file] of files.entries()) {
      /* Relative to the generated file, which sits in packages/bundle/gen. */
      const from = relative(dirname(join(ROOT, ENTRY)), join(ROOT, site.build, file))
      imports.push(
        `import ${identifier(fileIndex)} from ${JSON.stringify(from.split(/[\\/]/u).join('/'))} with { type: 'file' }`,
      )
      entries.push(`        [${JSON.stringify(file)}, ${identifier(fileIndex)}],`)
    }

    sites.push(
      [
        '    {',
        `      name: ${JSON.stringify(site.name)},`,
        `      basePath: ${JSON.stringify(site.basePath)},`,
        `      index: ${identifier(files.indexOf('index.html'))},`,
        '      files: new Map([',
        ...entries,
        '      ]),',
        '    },',
      ].join('\n'),
    )

    console.log(`bundle: ${site.name} — ${files.length} files from ${site.build}`)
  }

  const source = [
    '/* Generated by scripts/bundle.ts. Not edited, not committed — see packages/bundle/.gitignore.',
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

/**
 * turbo, through the bun that is running this script.
 *
 * `bun x` rather than the bare name, because the name finds whatever turbo is on the PATH —
 * on this machine a globally installed 2.5.8 next to the repository's pinned 2.10.5 — and a
 * build that silently uses a different version of the tool that owns the dependency graph is
 * a build nobody can reproduce.
 */
async function turbo(args: readonly string[], env: Record<string, string>): Promise<void> {
  await run([process.execPath, 'x', 'turbo', ...args], env)
}

async function run(command: readonly string[], env: Record<string, string>): Promise<void> {
  /* `bun` rather than the absolute path it was started from — the line is meant to be a
     command somebody can read, and retype. */
  console.log(
    `bundle: ${command.map((part) => (part === process.execPath ? 'bun' : part)).join(' ')}`,
  )
  const child = Bun.spawn(command, {
    cwd: ROOT,
    env: { ...process.env, ...env },
    stdio: ['inherit', 'inherit', 'inherit'],
  })
  const code = await child.exited
  if (code !== 0) {
    throw new Error(`${command.slice(1).join(' ')} failed with exit code ${code}`)
  }
}

function usage(): string {
  return `bun bundle — build the single Gradido executable

  --outfile=<path>  where to write it (default: build/gradido)
  --skip-build      do not build the frontends and the addons first
  -h, --help        this text

The frontends are built for the binary that serves them: API_BASE_URL empty, BASE_PATH set
to where each is mounted. Set API_BASE_URL in the environment to override the first.`
}

main().catch((error) => {
  console.error(error instanceof Error ? error.message : error)
  process.exit(1)
})
