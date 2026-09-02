import { afterEach, beforeEach, describe, expect, test } from 'bun:test'
import { Logger } from '@gradido/service-core'
import { openTestDatabase, type TestDatabase, testDatabaseKinds, testQuery } from '../../testing'
import { MIGRATIONS } from './migrations'
import { EMPTY_TARGET, migrateDown, runMigrations } from './runMigrations'

const silent = Logger.create({ LOG_LEVEL: 'fatal', LOG_FILE: '', NODE_ENV: 'test' })

for (const kind of testDatabaseKinds()) {
  describe(`migrateDown (${kind})`, () => {
    let database: TestDatabase

    beforeEach(async () => {
      /* openTestDatabase has already migrated up, so this starts at the head. */
      database = await openTestDatabase(kind)
    })

    afterEach(async () => {
      await database.close()
    })

    const versions = async () =>
      (await testQuery(database.connection, 'SELECT version FROM migrations ORDER BY version')).map(
        (row) => Number(row.version),
      )

    const tables = async () =>
      (
        await testQuery(
          database.connection,
          kind === 'sqlite'
            ? "SELECT name FROM sqlite_master WHERE type = 'table'"
            : "SELECT tablename AS name FROM pg_tables WHERE schemaname = 'public'",
        )
      ).map((row) => String(row.name))

    test('undoes the last migration and forgets its row', async () => {
      const undone = await migrateDown(database.connection, silent)

      expect(undone.name).toBe(MIGRATIONS.at(-1)?.name ?? '')
      expect(await versions()).toEqual(MIGRATIONS.slice(0, -1).map((m) => m.version))
      expect(await tables()).not.toContain('users')
    })

    // The whole point of the design: three steps is three decisions, not one argument.
    test('undoes one step per run, however many are applied', async () => {
      await migrateDown(database.connection, silent)
      expect(await versions()).toHaveLength(MIGRATIONS.length - 1)

      await migrateDown(database.connection, silent)
      expect(await versions()).toHaveLength(MIGRATIONS.length - 2)
    })

    /** One migration lower than the head — what a confirmation has to name. */
    const oneLower = MIGRATIONS.at(-2)?.name ?? EMPTY_TARGET

    test('refuses a confirmation that is not one migration lower', async () => {
      expect(
        migrateDown(database.connection, silent, { target: 'something_else' }),
      ).rejects.toThrow(/the confirmation names "something_else". Nothing was undone/u)

      expect(await versions()).toHaveLength(MIGRATIONS.length)
    })

    // Naming the migration being undone rather than the one to end at is the likely slip,
    // and it is refused: the value describes where the database lands, not what it loses.
    test('refuses a confirmation that names the head instead of the target', async () => {
      const head = MIGRATIONS.at(-1)

      expect(migrateDown(database.connection, silent, { target: head?.name })).rejects.toThrow(
        /Nothing was undone/u,
      )
    })

    test('runs when the confirmation names one migration lower', async () => {
      await migrateDown(database.connection, silent, { target: oneLower })

      expect(await versions()).toHaveLength(MIGRATIONS.length - 1)
    })

    // The confirmation disarms itself: once that target is reached it is no longer one lower,
    // so a value left behind in an env file cannot take the next step too.
    test('the same confirmation does not work twice', async () => {
      await migrateDown(database.connection, silent, { target: oneLower })

      expect(migrateDown(database.connection, silent, { target: oneLower })).rejects.toThrow(
        /Nothing was undone/u,
      )
    })

    test('undoing the first migration is confirmed with an empty database', async () => {
      for (const _migration of MIGRATIONS.slice(1)) {
        await migrateDown(database.connection, silent)
      }

      await migrateDown(database.connection, silent, { target: EMPTY_TARGET })
      expect(await versions()).toEqual([])
    })

    test('refuses when there is nothing left to undo', async () => {
      for (const _migration of MIGRATIONS) {
        await migrateDown(database.connection, silent)
      }

      expect(migrateDown(database.connection, silent)).rejects.toThrow(/nothing to undo/u)
    })

    test('what it undid can be migrated up again', async () => {
      await migrateDown(database.connection, silent)
      await runMigrations(database.connection, silent)

      expect(await versions()).toEqual(MIGRATIONS.map((migration) => migration.version))
      expect(await tables()).toContain('users')
    })
  })
}
