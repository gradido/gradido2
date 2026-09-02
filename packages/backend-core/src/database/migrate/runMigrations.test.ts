import { afterEach, beforeEach, describe, expect, test } from 'bun:test'
import { Logger } from '@gradido/service-core'
import { openTestDatabase, type TestDatabase, testDatabaseKinds, testQuery } from '../../testing'
import { MIGRATIONS } from './migrations'
import { runMigrations, SCHEMA_VERSION } from './runMigrations'

const silent = Logger.create({ LOG_LEVEL: 'fatal', LOG_FILE: '', NODE_ENV: 'test' })

/* What the contract's migrations look like is asserted in migrations.test.ts, next to the
   loader that produces them. This is only what the runner adds. */
describe('SCHEMA_VERSION', () => {
  test('is the last migration this build carries', () => {
    expect(SCHEMA_VERSION).toBe(MIGRATIONS.length)
  })
})

for (const kind of testDatabaseKinds()) {
  describe(`runMigrations (${kind})`, () => {
    // openTestDatabase has already migrated: these assert on what it left behind, and on
    // what a second run does to it.
    let database: TestDatabase

    beforeEach(async () => {
      database = await openTestDatabase(kind)
    })

    afterEach(async () => {
      await database.close()
    })

    test('records every migration it applied', async () => {
      const rows = await testQuery(database.connection, 'SELECT * FROM migrations ORDER BY version')
      expect(rows.map((row) => Number(row.version))).toEqual(
        MIGRATIONS.map((migration) => migration.version),
      )
      expect(rows.map((row) => row.file_name)).toEqual(
        MIGRATIONS.map((migration) => migration.name),
      )
    })

    test('does nothing the second time, and says which version it found', async () => {
      const from = await runMigrations(database.connection, silent)

      expect(from).toBe(SCHEMA_VERSION)
      const rows = await testQuery(database.connection, 'SELECT * FROM migrations')
      expect(rows).toHaveLength(MIGRATIONS.length)
    })
  })
}
