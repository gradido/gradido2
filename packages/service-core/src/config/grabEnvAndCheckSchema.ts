import * as v from 'valibot'
import { Logger } from '../logging'
import { resolveSecrets, SECRET_VARIABLES, SecretUnreadable } from './secret'

/** What `startup.config.failed` carries -- contracts/logging.json. */
export type ConfigFailure = {
  readonly variable: string
  readonly reason: 'invalid' | 'unreadable'
  readonly message: string
}

/**
 * Which variable is wrong and why, out of whatever reading the configuration threw.
 *
 * The first issue only: a configuration is fixed one variable at a time, and the next start
 * names the next one.
 */
export function configFailureOf(error: unknown): ConfigFailure {
  if (error instanceof SecretUnreadable) {
    return { variable: error.variable, reason: 'unreadable', message: error.message }
  }
  if (error instanceof v.ValiError) {
    const issue = error.issues[0]
    const variable = String(issue.path?.[0]?.key ?? '')
    /* `received` only for a primitive that is not a secret. A rule that spans several
       variables is checked on the whole config and gets that object as its input — printing
       it would be noise at best, and one valibot release away from putting DB_PASSWORD in the
       log at worst. The variable the rule was forwarded onto is already named by the path. */
    const quotable =
      !(issue.input !== null && typeof issue.input === 'object') &&
      !(SECRET_VARIABLES as readonly string[]).includes(variable)
    const received = quotable ? `, received: ${issue.received}` : ''
    return { variable, reason: 'invalid', message: `${variable}: ${issue.message}${received}` }
  }
  return {
    variable: '',
    reason: 'invalid',
    message: error instanceof Error ? error.message : String(error),
  }
}

/**
 * Parses process.env against a schema and exits if it does not fit.
 *
 * A misconfigured server must not start half-working: a missing port or an unreadable
 * database name is discovered here, at boot, with the offending variable named, rather
 * than on the first request that happens to need it.
 *
 * It is said as `startup.config.failed`, on a logger made for that one line: the service's own
 * logger is configured by what failed to parse. Its level is the default and it writes to
 * stdout only, which is where a process that never started is looked for.
 */
export function grabEnvAndCheckBySchema<
  const TSchema extends v.BaseSchema<unknown, unknown, v.BaseIssue<unknown>>,
>(schema: TSchema): v.InferOutput<TSchema> {
  try {
    /* The secrets first: a password may come from a systemd credential or from a file the
       environment names, and only lastly from the variable itself -- contracts/secrets.json.
       What the schema sees is therefore not what /proc/<pid>/environ says, which is the point
       of the whole order. A source that is named and unreadable throws from here and is caught
       below, with its own sentence rather than a schema issue. */
    return v.parse(schema, resolveSecrets(process.env))
  } catch (error) {
    const failure = configFailureOf(error)
    const logger = Logger.create({
      LOG_LEVEL: 'info',
      LOG_FILE: '',
      NODE_ENV: process.env.NODE_ENV === 'development' ? 'development' : 'production',
    })
    logger.fatal(
      {
        cat: 'startup',
        event: 'startup.config.failed',
        data: { variable: failure.variable, reason: failure.reason },
      },
      failure.message,
    )
    logger.flush()
    process.exit(1)
  }
}
