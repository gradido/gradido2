import { Database as SqliteDatabase } from 'bun:sqlite'
import { sql } from 'drizzle-orm'
import { type BunSQLDatabase, drizzle as drizzlePostgres } from 'drizzle-orm/bun-sql/postgres'
import { drizzle as drizzleSqlite, type SQLiteBunDatabase } from 'drizzle-orm/bun-sqlite'
import { type DatabaseConfig, isUnixSocketHost } from './schema'

/**
 * The database, plus which one it is.
 *
 * The two are one discriminated union rather than one common interface on purpose: the
 * SQL dialects are not the same, and a repository that has to know which one it is talking
 * to should have to say so. PostgreSQL is the reference; SQLite mirrors what it lacks with
 * simpler queries, and that decision belongs in the repository, not behind a facade that
 * hides it.
 */
export type DatabaseConnection =
  | {
      readonly kind: 'postgresql'
      readonly drizzle: BunSQLDatabase
      /** Asks the database whether it is there. Throws what the driver throws. */
      readonly probe: () => Promise<void>
      readonly close: () => Promise<void>
    }
  | {
      readonly kind: 'sqlite'
      readonly drizzle: SQLiteBunDatabase
      readonly probe: () => Promise<void>
      readonly close: () => Promise<void>
    }

/**
 * Opens the database named by the environment.
 *
 * Nothing is contacted here -- both drivers connect lazily. Whether the database answers
 * is a separate question, asked by `waitForDatabase` at startup, because the answer may
 * be "not yet" and that is worth waiting for. The choice of database is a startup
 * decision and cannot change while running.
 *
 * **drizzle's `jit` stays off, on both.** It compiles a row mapper with `new Function` for
 * every query built, which pays back on a prepared statement or a large result and costs
 * on everything else — and everything else is what this package does: a few rows per
 * query, built where it runs. Measured on drizzle 1.0.0-rc.4, one joined row out of SQLite
 * took 64 µs without it and 80 µs with it; only a 1000-row select got faster. Turn it on
 * together with `.prepare()`, not instead of it.
 */
export function connectDatabase(env: DatabaseConfig): DatabaseConnection {
  if (env.DB_TYPE === 'sqlite') {
    const sqlite = new SqliteDatabase(env.DB_FILE, { create: true, strict: true })
    /* WAL lets readers and one writer work at the same time, which is the whole point of
       using SQLite for a small community rather than a toy. foreign_keys is off by default
       in SQLite and must be switched on per connection. */
    sqlite.exec('PRAGMA journal_mode = WAL')
    sqlite.exec('PRAGMA foreign_keys = ON')

    const sqliteDrizzle = drizzleSqlite({ client: sqlite })

    return {
      kind: 'sqlite',
      drizzle: sqliteDrizzle,
      probe: async () => {
        sqliteDrizzle.run(sql`select 1`)
      },
      close: async () => {
        sqlite.close(false)
      },
    }
  }

  /*
   * `path` or `host`, decided by the one variable. bun's client has two options where libpq has
   * one: it does not read a leading '/' in `host` as a socket directory, so the branch that libpq
   * makes for itself is made here. The port goes in either way -- a socket connection reads it
   * too, because the socket file is named `<dir>/.s.PGSQL.<port>`.
   *
   * See contracts/database-config.json, rules.connection, which is where the two spellings of
   * this one idea are written down.
   */
  const postgres = drizzlePostgres({
    connection: {
      ...(isUnixSocketHost(env.DB_HOST) ? { path: env.DB_HOST } : { host: env.DB_HOST }),
      port: env.DB_PORT,
      user: env.DB_USER,
      password: env.DB_PASSWORD,
      database: env.DB_DATABASE,
      /* bun opens these on demand, up to this many, and queues a query when all are busy -- the
         same limit the C path opens up front. contracts/database-config.json, rules.pool. */
      max: env.DB_POOL_SIZE,
    },
  })

  return {
    kind: 'postgresql',
    drizzle: postgres,
    probe: async () => {
      await postgres.execute(sql`select 1`)
    },
    close: async () => {
      await postgres.$client.close()
    },
  }
}
