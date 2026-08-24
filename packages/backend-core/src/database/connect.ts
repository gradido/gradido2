import { Database as SqliteDatabase } from 'bun:sqlite'
import { sql } from 'drizzle-orm'
import { type BunSQLDatabase, drizzle as drizzlePostgres } from 'drizzle-orm/bun-sql'
import { type BunSQLiteDatabase, drizzle as drizzleSqlite } from 'drizzle-orm/bun-sqlite'
import type { DatabaseEnv } from './schema'

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
      readonly drizzle: BunSQLiteDatabase
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
 */
export function connectDatabase(env: DatabaseEnv): DatabaseConnection {
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

  const postgres = drizzlePostgres({
    connection: {
      host: env.DB_HOST,
      port: env.DB_PORT,
      user: env.DB_USER,
      password: env.DB_PASSWORD,
      database: env.DB_DATABASE,
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
