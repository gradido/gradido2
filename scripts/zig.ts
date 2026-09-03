/**
 * `bun run zig …` — the C build, without a zig on the machine.
 *
 * `fast-servers/build.zig` needs a Zig toolchain, and a developer who only wants to work on the
 * TypeScript path should not have to install one to check that the other path still compiles.
 * They do not have to: `c-cpp-zig-build` already downloads a pinned Zig for `shared-native` and
 * `email-native`, verifies the archive against the SHA-256 ziglang.org publishes, and caches it
 * in `~/.zig-build` across every checkout on the machine. This hands the same toolchain to
 * `fast-servers`.
 *
 * **One pin, one place**, which is what `AGENTS.md` section 12 asks for: the version comes out
 * of that package rather than out of whatever a machine happens to have installed, so two
 * developers and CI compile the fast path with the same compiler. `build.zig.zon` states a
 * *floor* of 0.15.1 — a floor is not a pin, and this is where the pin comes from.
 *
 *   bun run zig build                    the binary, in fast-servers/zig-out/bin
 *   bun run zig build -Dtests test       and its tests
 *   bun run zig version                  which toolchain that is
 *
 * A system zig still works and is not asked to leave: `zig build` in `fast-servers/` is the
 * documented command there and stays the shortest way to work on the C. This is the way in for
 * a machine that has no zig, and it is what `bun bundle` uses when it is asked for the C binary.
 */
import { resolve } from 'node:path'
import { run } from './run'

const ROOT = resolve(import.meta.dirname, '..')

/**
 * The managed toolchain, downloaded on first use.
 *
 * Imported here rather than at the top of the file so that a script that never asks for zig
 * never loads the package — `bundle.ts` calls this only when `BUNDLE_C` says so.
 */
export async function zigExe(): Promise<string> {
  const { resolveZig } = await import('c-cpp-zig-build')
  const zig = await resolveZig()
  return zig.exe
}

/** Runs the managed toolchain against `fast-servers/`, which is the only zig build in here. */
export async function zig(args: readonly string[]): Promise<void> {
  await run([await zigExe(), ...args], {}, resolve(ROOT, 'fast-servers'))
}

if (import.meta.main) {
  const argv = process.argv.slice(2)
  await zig(argv.length === 0 ? ['build'] : argv)
}
