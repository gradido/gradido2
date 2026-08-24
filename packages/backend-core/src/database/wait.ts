import { type Logger, retry } from 'service-core'
import type { DatabaseConnection } from './connect'

/* A database and the service that uses it start together, and the database is the slower
   of the two: PostgreSQL replays its write-ahead log before it answers, and under docker
   compose it may not even have been started yet. Thirty seconds covers that and is still
   short enough that a genuinely absent database is reported while someone is watching. */
const CONNECT_ATTEMPTS = 30
const CONNECT_DELAY_MS = 1000
/* SQLite is opened when the connection is created, so there is no second party to wait
   for: a failure now is a broken or unreadable file and will still be one in a second. */
const SQLITE_ATTEMPTS = 1
/* A refused connection comes back in milliseconds; this is for the host that answers
   nothing at all, where the socket would otherwise sit there for minutes. */
const CONNECT_TIMEOUT_MS = 5000

/**
 * PostgreSQL error classes that say the server heard the question and refused it. Waiting
 * does not turn a wrong password into a right one, so these end the startup immediately
 * with the driver's own message instead of half a minute later.
 *
 * Bun's SQL driver puts the five-character SQLSTATE in `errno` and leaves it absent when
 * the server never answered -- which is exactly the case that is worth retrying. Drizzle
 * wraps what the driver threw, so both the SQLSTATE and the readable sentence live one or
 * more `cause` links down rather than on the error that arrives here.
 */
const PERMANENT_SQLSTATE_CLASSES = [
  '28', // invalid authorization: wrong password, no such role, rejected by pg_hba.conf
  '3D', // invalid catalog name: the database does not exist
  '42', // syntax error or access rule violation: no permission for what was asked
]

/** An error and everything it was caused by. Bounded, because a cause can be a cycle. */
function* causeChain(error: unknown): Generator<Record<string, unknown>> {
  let current = error
  for (let depth = 0; depth < 8; depth++) {
    if (typeof current !== 'object' || current === null) {
      return
    }
    yield current as Record<string, unknown>
    current = (current as { cause?: unknown }).cause
  }
}

function sqlStateOf(error: unknown): string | undefined {
  for (const link of causeChain(error)) {
    const errno = link.errno
    if (typeof errno === 'string' && errno.length === 5) {
      return errno
    }
  }
  return undefined
}

/**
 * What the driver said, on one line.
 *
 * The deepest cause is the one that knows something -- the wrapper above it only repeats
 * the query. Whitespace is collapsed because a log line is a line.
 */
export function databaseErrorMessage(error: unknown): string {
  let message = String(error)
  let code: string | undefined
  for (const link of causeChain(error)) {
    if (typeof link.message === 'string' && link.message.length > 0) {
      message = link.message
      code = typeof link.code === 'string' ? link.code : undefined
    }
  }
  const oneLine = message.replace(/\s+/gu, ' ').trim()
  return code === undefined ? oneLine : `${oneLine} (${code})`
}

/** True while the failure still looks like "not yet" rather than "not like this". */
export function isDatabaseStartingUp(error: unknown): boolean {
  const sqlState = sqlStateOf(error)
  if (sqlState === undefined) {
    /* No SQLSTATE means no answer: refused, unresolvable, or timed out. All of those are
       what a database that is still coming up looks like from here. */
    return true
  }
  return !PERMANENT_SQLSTATE_CLASSES.includes(sqlState.slice(0, 2))
}

/** Overrides for tests and for callers that know better than the constants above. */
export type WaitForDatabaseOptions = {
  attempts?: number
  delayMs?: number
  timeoutMs?: number
}

/**
 * Waits until the database answers, and throws what the driver threw if it will not.
 */
export async function waitForDatabase(
  connection: DatabaseConnection,
  logger: Logger,
  options: WaitForDatabaseOptions = {},
): Promise<void> {
  const attempts =
    options.attempts ?? (connection.kind === 'sqlite' ? SQLITE_ATTEMPTS : CONNECT_ATTEMPTS)
  const delayMs = options.delayMs ?? CONNECT_DELAY_MS

  await retry(connection.probe, {
    attempts,
    delayMs,
    timeoutMs: options.timeoutMs ?? CONNECT_TIMEOUT_MS,
    label: `${connection.kind} probe`,
    shouldRetry: isDatabaseStartingUp,
    onRetry: ({ attempt, error, delayMs }) => {
      logger.warn(
        { cat: 'db', event: 'db.connection.failed', data: { attempt, attempts } },
        `database not reachable yet, retrying in ${delayMs}ms: ${databaseErrorMessage(error)}`,
      )
    },
  })
}
