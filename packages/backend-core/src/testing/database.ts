import { Logger } from '@gradido/service-core'
import { sql } from 'drizzle-orm'
import { connectDatabase, type DatabaseConnection, runMigrations } from '../database'

/**
 * A migrated, empty database to run a test against — on whichever dialects this machine can
 * offer.
 *
 * `Architecture.md` asks for the tests to run against both database modes, and the honest
 * shape of that on a developer's machine is: SQLite always, because it needs nothing;
 * PostgreSQL when somebody has pointed the environment at a database that may be emptied.
 * A test written through {@link testDatabaseKinds} therefore covers both without being two
 * tests, and covers one without failing on a laptop that has no server running.
 *
 * ```sh
 * TEST_DB_POSTGRES=1 DB_DATABASE=gradido_test DB_USER=… DB_PASSWORD=… bun run test
 * ```
 */
export type TestDatabase = {
  readonly connection: DatabaseConnection
  readonly close: () => Promise<void>
}

/** Everything migration 0001 creates, plus the bookkeeping. Emptied before a run. */
const MIGRATED_TABLES = ['user_contacts', 'users', 'migrations']

/**
 * A database name has to say it is a test database.
 *
 * This helper drops tables. The name is the only thing standing between that and somebody's
 * development data, so it is checked rather than trusted — an opt-in flag alone would not
 * survive being set once and forgotten.
 */
const TEST_DATABASE_SUFFIX = '_test'

export function testDatabaseKinds(): DatabaseConnection['kind'][] {
  return process.env.TEST_DB_POSTGRES === undefined ? ['sqlite'] : ['sqlite', 'postgresql']
}

export async function openTestDatabase(kind: DatabaseConnection['kind']): Promise<TestDatabase> {
  /* Quiet unless something goes wrong: a test that passes should print nothing, and the
     migration runner logs a line per migration at info. */
  const logger = Logger.create({ LOG_LEVEL: 'fatal', LOG_FILE: '', NODE_ENV: 'test' })

  if (kind === 'sqlite') {
    /* ':memory:' is a fresh, private database per connection — nothing to clean up, and no
       file left behind by a test that failed halfway. */
    const connection = connectDatabase({
      ...emptyPostgresEnv,
      DB_TYPE: 'sqlite',
      DB_FILE: ':memory:',
    })
    await runMigrations(connection, logger)
    return { connection, close: connection.close }
  }

  const database = process.env.DB_DATABASE ?? ''
  if (!database.endsWith(TEST_DATABASE_SUFFIX)) {
    throw new Error(
      `refusing to run tests against "${database}": DB_DATABASE must end in ${TEST_DATABASE_SUFFIX}`,
    )
  }

  const connection = connectDatabase({
    DB_TYPE: 'postgresql',
    DB_HOST: process.env.DB_HOST ?? 'localhost',
    DB_PORT: Number(process.env.DB_PORT ?? 5432),
    DB_USER: process.env.DB_USER ?? 'gradido',
    DB_PASSWORD: process.env.DB_PASSWORD ?? '',
    DB_DATABASE: database,
    DB_FILE: '',
  })
  /* Always true — connectDatabase was just told which database this is — but the union is
     what carries the driver's type, and narrowing it is cheaper than asserting past it. */
  if (connection.kind === 'postgresql') {
    for (const table of MIGRATED_TABLES) {
      await connection.drizzle.execute(sql.raw(`DROP TABLE IF EXISTS ${table} CASCADE`))
    }
  }
  await runMigrations(connection, logger)
  return { connection, close: connection.close }
}

/** The PostgreSQL half of the environment, unused when the answer is SQLite. */
const emptyPostgresEnv = {
  DB_HOST: '',
  DB_PORT: 0,
  DB_USER: '',
  DB_PASSWORD: '',
  DB_DATABASE: '',
} as const

/**
 * Rows of a raw statement, whichever driver is underneath.
 *
 * For tests only, and deliberately untyped: what a test asserting on the schema wants to see
 * is the row the *database* holds, not the row a drizzle table definition says it should
 * hold — those are the two things that can drift apart.
 */
export async function testQuery(
  connection: DatabaseConnection,
  statement: string,
): Promise<Record<string, unknown>[]> {
  if (connection.kind === 'sqlite') {
    return connection.drizzle.all(sql.raw(statement))
  }
  return (await connection.drizzle.execute(sql.raw(statement))) as Record<string, unknown>[]
}
