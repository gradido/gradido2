import type { Database } from 'bun:sqlite'
import { CriticalError } from '@gradido/service-core'
import type { RunTransaction } from './connect'

/** What a transaction needs of bun:sqlite's Database. A test hands in its own. */
export type SqliteTransactionClient = Pick<Database, 'exec' | 'inTransaction'>

/**
 * `BEGIN`, the work, then `COMMIT` or `ROLLBACK`, on the one SQLite connection.
 *
 * Not drizzle's own transaction: bun:sqlite's takes a synchronous callback and commits when
 * it returns, which is before an asynchronous one has done anything.
 *
 * What the work threw is what the caller gets, whatever the rollback then does -- with one
 * exception. A rollback that fails and leaves the transaction open leaves the only connection
 * inside it: every later write would join a transaction nobody ends. That is a
 * {@link CriticalError}, and the process stops.
 */
export function sqliteTransaction(sqlite: SqliteTransactionClient): RunTransaction {
  return async (work) => {
    sqlite.exec('BEGIN')
    try {
      const result = await work({ kind: 'sqlite' })
      sqlite.exec('COMMIT')
      return result
    } catch (error) {
      try {
        /* Asked first: a failed COMMIT, or SQLite itself, may have ended the transaction. */
        if (sqlite.inTransaction) {
          sqlite.exec('ROLLBACK')
        }
      } catch (rollbackError) {
        if (isStuckInTransaction(sqlite)) {
          throw new CriticalError(
            { cat: 'db', event: 'db.transaction.failed', data: { db: 'sqlite', step: 'rollback' } },
            'the rollback failed and the transaction is still open',
            [error, rollbackError],
          )
        }
      }
      throw error
    }
  }
}

/** False for a connection that is closed, which is where asking throws: nothing is stuck then. */
function isStuckInTransaction(sqlite: SqliteTransactionClient): boolean {
  try {
    return sqlite.inTransaction
  } catch {
    return false
  }
}
