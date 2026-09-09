import { readFileSync } from 'node:fs'
import { join } from 'node:path'

/**
 * Where a secret comes from, and in which order — `contracts/secrets.json` is normative.
 *
 * A secret in an environment variable is readable in `/proc/<pid>/environ`, is inherited by
 * every child process, and turns up in `docker inspect`, in crash dumps and in CI logs. So the
 * environment may carry a *path* to a secret, and the best of the three sources carries no
 * environment entry at all.
 *
 * Nothing here is systemd-specific, and that is what makes the mechanism usable from both
 * implementations: `LoadCredential=` places the secret as a file in a per-unit directory on a
 * tmpfs and names that directory in `$CREDENTIALS_DIRECTORY`. Reading it is reading a file at a
 * path the process was told. There is no API to bind against and no platform check to write —
 * the variable is simply unset everywhere that is not a systemd service.
 */

/**
 * Every value this project treats as a secret.
 *
 * Declared rather than discovered, and short because the services are. A secret added to either
 * implementation is added to `contracts/secrets.json` first: an operator who has learned
 * `DB_PASSWORD_FILE` has learned the mechanism, and a second secret that resolved differently
 * would make that knowledge wrong. The mail relay's password and the JWT signing key are the
 * two this was written in anticipation of.
 */
export const SECRET_VARIABLES = ['DB_PASSWORD'] as const

export type SecretVariable = (typeof SECRET_VARIABLES)[number]

/** One trailing line ending and nothing else — see contracts/secrets.json, resolution.rules. */
function stripOneLineEnding(content: string): string {
  if (content.endsWith('\r\n')) {
    return content.slice(0, -2)
  }
  return content.endsWith('\n') ? content.slice(0, -1) : content
}

/**
 * The secret @p name, from the best source that has it, or undefined when none has it.
 *
 * A source that is *named* and cannot be read throws rather than falling back: an unreadable
 * secret must not quietly become an empty one, because an empty password is how a process
 * connects as somebody else or fails much later about the wrong thing.
 */
function resolveSecret(name: string, env: Record<string, string | undefined>): string | undefined {
  const credentials = env.CREDENTIALS_DIRECTORY
  if (credentials !== undefined && credentials !== '') {
    try {
      return stripOneLineEnding(readFileSync(join(credentials, name), 'utf8'))
    } catch (error) {
      /* Absent is ordinary — a unit loads the credentials it needs and no others. Present and
         unreadable is not, and the difference is the errno. */
      if ((error as NodeJS.ErrnoException).code !== 'ENOENT') {
        throw new Error(
          `the systemd credential ${join(credentials, name)} could not be read: ${String(error)}`,
        )
      }
    }
  }

  const path = env[`${name}_FILE`]
  if (path !== undefined && path !== '') {
    try {
      return stripOneLineEnding(readFileSync(path, 'utf8'))
    } catch (error) {
      throw new Error(
        `${name}_FILE names ${path}, which could not be read — refusing to fall back to ${name}: ${String(error)}`,
      )
    }
  }

  return env[name]
}

/**
 * The environment with every secret resolved into it, ready to be parsed against a schema.
 *
 * A copy rather than a mutation of `process.env`: a secret that was deliberately kept out of the
 * environment has no business being put back into it, where the next child process would inherit
 * it. What the schema sees is not what `/proc/<pid>/environ` says, and that is the point.
 */
export function resolveSecrets(
  env: Record<string, string | undefined>,
): Record<string, string | undefined> {
  const resolved: Record<string, string | undefined> = { ...env }
  for (const name of SECRET_VARIABLES) {
    resolved[name] = resolveSecret(name, env)
  }
  return resolved
}
