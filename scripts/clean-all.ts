import { readdir, readFile, rm } from 'node:fs/promises'
import { dirname, join, resolve } from 'node:path'

type PackageJson = {
  workspaces?: string[]
}

const ROOT = resolve(import.meta.dirname, '..')

const TARGETS = [
  'node_modules',
  '.turbo',
  'build',
  '.zig-cache',
  '.zig-native',
  'compile_commands.json',
]

/**
 * Directories with a native build of their own.
 *
 * They are listed rather than discovered because they are not workspaces and never will be:
 * nothing under `fast-servers/` is in the root package.json, deliberately, so that
 * `bun install` never needs the C path -- see AGENTS.md, "droppable, not merely removable".
 * The workspace loop therefore never reaches them.
 *
 * A name that is not there is not an error: removeIfExists treats a missing path as already
 * clean, which is what keeps this list from breaking the day the fast path is dropped.
 */
const NATIVE_ROOTS = ['fast-servers']

/**
 * What a native build leaves behind, beside build.zig and CMakeLists.txt.
 *
 * zig writes `.zig-cache` and `zig-out`, and regenerates `compile_commands.json` on every
 * build. cmake writes into `build/` when it is pointed there, which is what the README does and
 * what `-p build/fallback` puts the second zig build into -- and leaves `CMakeCache.txt` and
 * `CMakeFiles/` in the source tree when someone configures without `-B`.
 */
const NATIVE_TARGETS = [
  '.zig-cache',
  'zig-out',
  'build',
  'compile_commands.json',
  'CMakeCache.txt',
  'CMakeFiles',
]

async function readWorkspaces(): Promise<string[]> {
  const packageJsonPath = join(ROOT, 'package.json')
  const packageJson = JSON.parse(await readFile(packageJsonPath, 'utf8')) as PackageJson

  if (!packageJson.workspaces) {
    throw new Error(`No "workspaces" entry found in ${packageJsonPath}`)
  }

  return packageJson.workspaces
}

/**
 * Expands the one glob shape bun workspaces actually use here: a trailing "/*".
 * Anything else is reported rather than silently skipped -- a workspace that is not
 * cleaned looks exactly like a workspace that was already clean.
 */
async function expand(pattern: string): Promise<string[]> {
  if (!pattern.includes('*')) {
    return [pattern]
  }

  if (!pattern.endsWith('/*') || pattern.slice(0, -2).includes('*')) {
    throw new Error(`Unsupported workspace pattern: ${pattern}`)
  }

  const parent = pattern.slice(0, -2)
  const entries = await readdir(join(ROOT, parent), { withFileTypes: true })

  return entries.filter((entry) => entry.isDirectory()).map((entry) => join(parent, entry.name))
}

async function removeIfExists(path: string): Promise<void> {
  try {
    await rm(path, {
      recursive: true,
      force: false,
    })

    console.log(`✓ ${path}`)
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') {
      return
    }
    console.error(`✗ Failed to remove ${path}`)
    throw error
  }
}

const patterns = await readWorkspaces()
const workspaces = (await Promise.all(patterns.map(expand))).flat()

console.log(`Cleaning ${workspaces.length} workspaces...\n`)

for (const workspace of workspaces) {
  const workspacePath = join(ROOT, workspace)

  console.log(`Workspace: ${workspace}`)

  for (const target of TARGETS) {
    await removeIfExists(join(workspacePath, target))
  }

  console.log()
}

console.log(`Cleaning ${NATIVE_ROOTS.length} native builds...\n`)

for (const nativeRoot of NATIVE_ROOTS) {
  console.log(`Native: ${nativeRoot}`)

  for (const target of NATIVE_TARGETS) {
    await removeIfExists(join(ROOT, nativeRoot, target))
  }

  console.log()
}

/**
 * What only the repository root has.
 *
 * `publish/` is the built frontends both servers embed -- see scripts/publish.ts. It belongs to
 * neither implementation, which is why it is here rather than in the per-workspace list.
 */
const ROOT_TARGETS = [...TARGETS, 'publish']

// Also clean root-level targets.
console.log('Root:')

for (const target of ROOT_TARGETS) {
  await removeIfExists(join(ROOT, target))
}

console.log('\nClean complete.')
