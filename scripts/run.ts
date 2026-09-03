/**
 * Running something else, for the scripts that build things.
 *
 * Two of them do — `publish.ts` and `bundle.ts` — and both have to reach turbo, so the two
 * lines that get that right live here rather than in each.
 */

/**
 * turbo, through the bun that is running this script.
 *
 * `bun x` rather than the bare name, because the name finds whatever turbo is on the PATH — a
 * globally installed one next to the repository's pinned version — and a build that silently
 * used a different version of the tool that owns the dependency graph is a build nobody can
 * reproduce.
 */
export async function turbo(args: readonly string[], env: Record<string, string>): Promise<void> {
  await run([process.execPath, 'x', 'turbo', ...args], env)
}

export async function run(
  command: readonly string[],
  env: Record<string, string>,
  cwd?: string,
): Promise<void> {
  /* `bun` rather than the absolute path it was started from — the line is meant to be a
     command somebody can read, and retype. */
  const shown = command.map((part) => (part === process.execPath ? 'bun' : part)).join(' ')
  console.log(`> ${shown}`)

  const child = Bun.spawn(command, {
    cwd,
    env: { ...process.env, ...env },
    stdio: ['inherit', 'inherit', 'inherit'],
  })
  const code = await child.exited
  if (code !== 0) {
    throw new Error(`${command.slice(1).join(' ')} failed with exit code ${code}`)
  }
}
