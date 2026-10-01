import { Database } from 'bun:sqlite'
import { describe, expect, test } from 'bun:test'
import { CriticalError } from '@gradido/service-core'
import { type SqliteTransactionClient, sqliteTransaction } from './sqliteTransaction'

/** A connection that does what it is scripted to, and remembers what it was told. */
function scripted(script: {
  inTransaction: () => boolean
  fail?: Record<string, Error>
}): SqliteTransactionClient & { statements: string[] } {
  const statements: string[] = []
  return {
    statements,
    get inTransaction() {
      return script.inTransaction()
    },
    exec(statement: string) {
      statements.push(statement)
      const failure = script.fail?.[statement]
      if (failure !== undefined) {
        throw failure
      }
      return { changes: 0, lastInsertRowid: 0 }
    },
  } as SqliteTransactionClient & { statements: string[] }
}

const failed = new Error('the work failed')
const work = () => Promise.reject(failed)

describe('sqliteTransaction', () => {
  test('commits what the work returned', async () => {
    const sqlite = scripted({ inTransaction: () => true })

    expect(await sqliteTransaction(sqlite)(async () => 'done')).toBe('done')
    expect(sqlite.statements).toEqual(['BEGIN', 'COMMIT'])
  })

  test('rolls back and hands on what the work threw', async () => {
    const sqlite = scripted({ inTransaction: () => true })

    await expect(sqliteTransaction(sqlite)(work)).rejects.toBe(failed)
    expect(sqlite.statements).toEqual(['BEGIN', 'ROLLBACK'])
  })

  test('does not roll back a transaction a failed COMMIT has already ended', async () => {
    const full = new Error('database or disk is full')
    const sqlite = scripted({ inTransaction: () => false, fail: { COMMIT: full } })

    await expect(sqliteTransaction(sqlite)(async () => 'done')).rejects.toBe(full)
    expect(sqlite.statements).toEqual(['BEGIN', 'COMMIT'])
  })

  test('keeps what the work threw when the rollback fails but nothing is left open', async () => {
    /* Open when the rollback is attempted, ended by SQLite itself when it is asked again. */
    let asked = 0
    const sqlite = scripted({
      inTransaction: () => asked++ === 0,
      fail: { ROLLBACK: new Error('disk I/O error') },
    })

    await expect(sqliteTransaction(sqlite)(work)).rejects.toBe(failed)
    expect(sqlite.statements).toEqual(['BEGIN', 'ROLLBACK'])
  })

  test('keeps what the work threw when the database was closed underneath it', async () => {
    const sqlite = new Database(':memory:')
    const ending = sqliteTransaction(sqlite)(() => {
      sqlite.close(false)
      return work()
    })

    await expect(ending).rejects.toBe(failed)
  })

  test('is critical when the rollback fails and the transaction stays open', async () => {
    const io = new Error('disk I/O error')
    const sqlite = scripted({ inTransaction: () => true, fail: { ROLLBACK: io } })

    const error = await sqliteTransaction(sqlite)(work).catch((thrown: unknown) => thrown)

    expect(error).toBeInstanceOf(CriticalError)
    const critical = error as CriticalError
    expect(critical.errors).toEqual([failed, io])
    expect(critical.line).toEqual({
      cat: 'db',
      event: 'db.transaction.failed',
      data: { db: 'sqlite', step: 'rollback' },
    })
  })
})
