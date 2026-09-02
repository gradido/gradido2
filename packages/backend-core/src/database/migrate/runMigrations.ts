import type { Logger } from '@gradido/service-core'
import { asc, eq, sql } from 'drizzle-orm'
import type { DatabaseConnection } from '..'
import { migrationsPg, migrationsSqlite } from '../tables'
import {
  type AppliedMigration,
  findSchemaDivergence,
  type Migration,
  schemaDivergenceMessage,
} from './Migration'
import { MIGRATIONS } from './migrations'

/**
 * The bookkeeping table has to exist before it can say whether anything else does, so it is
 * the one piece of DDL that is not itself a migration. `IF NOT EXISTS` rather than a version
 * check for the same reason: there is nothing to ask yet.
 */
const MIGRATIONS_TABLE = {
  postgresql: `CREATE TABLE IF NOT EXISTS migrations (
    version integer NOT NULL PRIMARY KEY,
    file_name varchar(256),
    date timestamptz(3) NOT NULL DEFAULT now()
  )`,
  sqlite: `CREATE TABLE IF NOT EXISTS migrations (
    version INTEGER NOT NULL PRIMARY KEY,
    file_name TEXT,
    date INTEGER NOT NULL
  )`,
} as const

/**
 * The database was built by code this build is not, so nothing was applied.
 *
 * A distinct type because it is not a database that cannot be reached and not a migration
 * that failed: the schema is intact, it is simply not this one's. `db.migration.denied` has
 * already been logged in full when this is thrown, so whoever catches it should not describe
 * it again — the useful sentence is on that line, and it names where the down step lives.
 */
export class SchemaMismatchError extends Error {
  public constructor(message: string) {
    super(message)
    this.name = 'SchemaMismatchError'
  }
}

/** The version the code in this process expects. Nothing below it may be started against. */
export const SCHEMA_VERSION = MIGRATIONS.reduce(
  (highest, migration) => Math.max(highest, migration.version),
  0,
)

/**
 * Brings the database up to `SCHEMA_VERSION` and answers with the version it was at before.
 *
 * Migrating on startup is a decision, and `contracts/db/migrations.json` records that it is
 * still an open one: what a *second instance of this same implementation* does while this one
 * is halfway through is not answered here. The fast path is not one of those instances — the
 * two implementations are never started against one database at the same time, see
 * `AGENTS.md` — so what is missing is a lock between peers, not between languages. A single
 * reference backend is what this is correct for today.
 *
 * A version in the database this build does not know **is** an error, and it is refused here
 * rather than passed to the caller: applying anything on top of a schema built by other code
 * produces one that neither branch describes. See `findSchemaDivergence`.
 */
export async function runMigrations(
  connection: DatabaseConnection,
  logger: Logger,
): Promise<number> {
  await execute(connection, MIGRATIONS_TABLE[connection.kind])
  const applied = await appliedMigrations(connection)

  const divergence = findSchemaDivergence(applied, MIGRATIONS)
  if (divergence !== undefined) {
    const message = schemaDivergenceMessage(divergence)
    /* Reported here, in full, and thrown as a type the caller can recognise so it does not
       report it a second time under a heading about reaching the database. */
    logger.fatal(
      {
        cat: 'db',
        event: 'db.migration.denied',
        data: {
          version: divergence.applied.version,
          file: divergence.applied.name,
          expected: divergence.expected?.name ?? null,
          db: connection.kind,
        },
      },
      message,
    )
    throw new SchemaMismatchError(message)
  }

  const from = applied.at(-1)?.version ?? 0

  /* Nothing is logged when there is nothing to do. The startup line already says which
     database this is, and a line per boot saying that the schema is unchanged is the kind of
     noise that teaches people to stop reading logs. */
  for (const migration of MIGRATIONS.filter((migration) => migration.version > from)) {
    await apply(connection, migration, logger)
  }
  return from
}

/**
 * One migration, all of it or none of it.
 *
 * Both databases roll DDL back inside a transaction, which is what makes a half-created
 * table impossible rather than merely unlikely — and it is why the statements and the row in
 * `migrations` are written together: a schema that has changed without a row saying so is
 * the one state nothing can recover from.
 *
 * The transaction is opened through drizzle rather than with a `BEGIN` statement, because
 * Bun's PostgreSQL driver is a pool: a raw `BEGIN` and the `COMMIT` after it can land on two
 * different connections, and everything in between would be committed one statement at a
 * time without anything looking wrong.
 */
async function apply(
  connection: DatabaseConnection,
  migration: Migration,
  logger: Logger,
): Promise<void> {
  const started = Date.now()
  try {
    if (connection.kind === 'sqlite') {
      connection.drizzle.transaction((tx) => {
        for (const statement of migration.up.sqlite) {
          tx.run(sql.raw(statement))
        }
        tx.insert(migrationsSqlite)
          .values({ version: migration.version, fileName: migration.name, date: new Date() })
          .run()
      })
    } else {
      await connection.drizzle.transaction(async (tx) => {
        for (const statement of migration.up.postgresql) {
          await tx.execute(sql.raw(statement))
        }
        await tx
          .insert(migrationsPg)
          .values({ version: migration.version, fileName: migration.name, date: new Date() })
      })
    }
  } catch (error) {
    logger.error(
      {
        cat: 'db',
        event: 'db.migration.failed',
        data: { version: migration.version, file: migration.name, db: connection.kind },
      },
      `migration ${migration.name} failed and was rolled back`,
    )
    throw error
  }

  logger.info(
    {
      cat: 'db',
      event: 'db.migration.applied',
      data: {
        version: migration.version,
        file: migration.name,
        db: connection.kind,
        ms: Date.now() - started,
      },
    },
    `applied migration ${migration.name}`,
  )
}

/**
 * What this database says has been applied to it, oldest first.
 *
 * All of them, not just the highest: the check is that they are a prefix of what this build
 * carries, and a highest version alone cannot tell a database built by another branch from
 * one built by this one.
 */
async function appliedMigrations(connection: DatabaseConnection): Promise<AppliedMigration[]> {
  const rows =
    connection.kind === 'sqlite'
      ? connection.drizzle
          .select({ version: migrationsSqlite.version, name: migrationsSqlite.fileName })
          .from(migrationsSqlite)
          .orderBy(asc(migrationsSqlite.version))
          .all()
      : await connection.drizzle
          .select({ version: migrationsPg.version, name: migrationsPg.fileName })
          .from(migrationsPg)
          .orderBy(asc(migrationsPg.version))

  return rows.map((row) => ({ version: row.version, name: row.name ?? '' }))
}

/** One statement, on whichever database this is. The only place raw SQL is run outside a migration. */
async function execute(connection: DatabaseConnection, statement: string): Promise<void> {
  if (connection.kind === 'sqlite') {
    connection.drizzle.run(sql.raw(statement))
    return
  }
  await connection.drizzle.execute(sql.raw(statement))
}

/**
 * Undoes the last migration, and only the last one.
 *
 * **One step per run, as in legacy.** A down run is the operation with the worst failure
 * mode in this file — `0002_users` down destroys every account — and a loop makes the
 * difference between "I meant one" and "I meant all of them" a matter of an argument nobody
 * checks twice. Undoing three steps is running this three times, which is three decisions.
 *
 * Whether the caller is *allowed* to run it is not asked here. That is a deployment
 * question — see `@gradido/backend`, which lets development run it and asks a release to name
 * the version it means to end at — and this refuses only what is wrong regardless of who is
 * asking: a database built by other code, a step with no writable inverse, nothing left to
 * undo, or a confirmation that does not describe one step down.
 */
/** What a `target` is when the step being undone is the first one: nothing is left. */
export const EMPTY_TARGET = '0'

export type MigrateDownOptions = {
  /**
   * The migration the database is to end at — one below the head — or `'0'` for an empty
   * database. Refused when it is anything else.
   *
   * The target rather than the step being undone, because that is what somebody deciding to
   * go back is thinking about, and because it stops being true the moment it is reached: a
   * confirmation left behind cannot authorise the next step down.
   *
   * Checked before anything runs, which is the only place a confirmation is worth anything —
   * afterwards it can only report what has already happened.
   */
  readonly target?: string
}

export async function migrateDown(
  connection: DatabaseConnection,
  logger: Logger,
  options: MigrateDownOptions = {},
): Promise<Migration> {
  await execute(connection, MIGRATIONS_TABLE[connection.kind])
  const applied = await appliedMigrations(connection)

  const divergence = findSchemaDivergence(applied, MIGRATIONS)
  if (divergence !== undefined) {
    /* The same refusal as going up, and for a stronger reason: undoing a migration this
       build does not have would run the wrong SQL against the right table. */
    throw new SchemaMismatchError(schemaDivergenceMessage(divergence))
  }

  const head = MIGRATIONS[applied.length - 1]
  if (head === undefined) {
    throw new Error('nothing to undo: this database has no migrations applied')
  }
  if (options.target !== undefined) {
    /* One below the head is what a down step reaches. '0' is that when the head is the first
       migration — there is no migration named for an empty database. */
    const reached = MIGRATIONS[applied.length - 2]?.name ?? EMPTY_TARGET
    if (options.target !== reached) {
      throw new Error(
        `this database is at "${head.name}", so one migration lower is "${reached}" — the confirmation names "${options.target}". Nothing was undone.`,
      )
    }
  }
  if (head.down === undefined) {
    throw new Error(
      `migration ${head.version} "${head.name}" has no down step and cannot be undone: see contracts/migrations`,
    )
  }
  const down = head.down

  if (connection.kind === 'sqlite') {
    connection.drizzle.transaction((tx) => {
      for (const statement of down.sqlite) {
        tx.run(sql.raw(statement))
      }
      tx.delete(migrationsSqlite).where(eq(migrationsSqlite.version, head.version)).run()
    })
  } else {
    await connection.drizzle.transaction(async (tx) => {
      for (const statement of down.postgresql) {
        await tx.execute(sql.raw(statement))
      }
      await tx.delete(migrationsPg).where(eq(migrationsPg.version, head.version))
    })
  }

  logger.warn(
    {
      cat: 'db',
      event: 'db.migration.reverted',
      data: { version: head.version, file: head.name, db: connection.kind },
    },
    `undid migration ${head.name}; the database is now at version ${head.version - 1}`,
  )
  return head
}
