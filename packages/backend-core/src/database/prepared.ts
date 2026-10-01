import type { EmptyRelations } from 'drizzle-orm'
import type { BunSQLDatabase, BunSQLTransaction } from 'drizzle-orm/bun-sql/postgres'
import type { SQLiteBunDatabase } from 'drizzle-orm/bun-sqlite'
import type { DatabaseTransaction } from './connect'

/** What a PostgreSQL statement is built on: the pool, or the one connection of a transaction. */
export type PostgresqlExecutor = BunSQLDatabase | BunSQLTransaction<EmptyRelations>

/**
 * The prepared statements of one PostgreSQL repository, each built the first time it is asked
 * for and kept under its name -- so a repository method can hold its query where it runs it.
 *
 * Inside a transaction the statement is built again on that transaction's connection and not
 * kept: one drizzle prepared is tied to the client it was built on, and the pool's client would
 * run it on a connection of its own choosing. That costs the query builder and nothing at the
 * server, where bun prepares by statement text, per connection.
 */
export class PostgresqlStatements {
  private readonly pooled = new Map<string, unknown>()

  public constructor(private readonly db: BunSQLDatabase) {}

  /** The statement @p name, which must be unique within the repository: it is the key. */
  public prepared<Query extends { prepare: (name: string) => unknown }>(
    name: string,
    tx: DatabaseTransaction | undefined,
    build: (executor: PostgresqlExecutor) => Query,
  ): ReturnType<Query['prepare']> {
    if (tx !== undefined) {
      if (tx.kind !== 'postgresql') {
        throw new Error(`a ${tx.kind} transaction was handed to a PostgreSQL repository`)
      }
      return build(tx.drizzle).prepare(name) as ReturnType<Query['prepare']>
    }
    let statement = this.pooled.get(name)
    if (statement === undefined) {
      statement = build(this.db).prepare(name)
      this.pooled.set(name, statement)
    }
    return statement as ReturnType<Query['prepare']>
  }
}

/**
 * The same for SQLite, without the transaction: there is one connection, so the statement kept
 * here is the one a transaction runs.
 */
export class SqliteStatements {
  private readonly kept = new Map<string, unknown>()

  public constructor(private readonly db: SQLiteBunDatabase) {}

  /** The statement @p name, which must be unique within the repository: it is the key. */
  public prepared<Query extends { prepare: () => unknown }>(
    name: string,
    build: (db: SQLiteBunDatabase) => Query,
  ): ReturnType<Query['prepare']> {
    let statement = this.kept.get(name)
    if (statement === undefined) {
      statement = build(this.db).prepare()
      this.kept.set(name, statement)
    }
    return statement as ReturnType<Query['prepare']>
  }
}
