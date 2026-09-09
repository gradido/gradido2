import { existsSync, readFileSync } from 'node:fs'
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
 * one this was written in anticipation of, and the JWT signing key is the one still to come.
 */
export const SECRET_VARIABLES = ['DB_PASSWORD', 'EMAIL_PASSWORD'] as const

export type SecretVariable = (typeof SECRET_VARIABLES)[number]

/**
 * Which of the three sources a secret came from.
 *
 * `setup` is the caller this exists for. It offers the current value as the default for its
 * question and writes the answer into `.env` — which is right for a value that was in the
 * environment and wrong for one that was not: pressing Enter on a secret that systemd keeps on
 * a tmpfs would copy it into a file in the working directory, and a mechanism that can be
 * downgraded by answering a prompt is not one.
 */
export type SecretSource = 'credential' | 'file' | 'environment'

/** One trailing line ending and nothing else — see contracts/secrets.json, resolution.rules. */
function stripOneLineEnding(content: string): string {
  if (content.endsWith('\r\n')) {
    return content.slice(0, -2)
  }
  return content.endsWith('\n') ? content.slice(0, -1) : content
}

/**
 * Where the secret @p name would come from, or undefined when no source has it.
 *
 * The same walk `resolveSecret` makes, reporting the source instead of the value — the two can
 * never disagree about which one wins, because they are the same three checks in the same order.
 */
export function secretSource(
  name: string,
  env: Record<string, string | undefined> = process.env,
): SecretSource | undefined {
  const credentials = env.CREDENTIALS_DIRECTORY
  if (credentials !== undefined && credentials !== '' && existsSync(join(credentials, name))) {
    return 'credential'
  }
  const path = env[`${name}_FILE`]
  if (path !== undefined && path !== '') {
    return 'file'
  }
  return env[name] === undefined ? undefined : 'environment'
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
