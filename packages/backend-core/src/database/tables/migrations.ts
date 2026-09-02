import {
  integer as pgInteger,
  pgTable,
  timestamp as pgTimestamp,
  varchar,
} from 'drizzle-orm/pg-core'
import { integer, sqliteTable, text } from 'drizzle-orm/sqlite-core'

/**
 * `migrations` — `contracts/db/migrations.json`. Which schema version the database is at.
 *
 * It is contracted rather than infrastructure because both implementations open the same
 * database and both have to answer "is this schema new enough for me" the same way. The
 * contract's open question — who is allowed to migrate, and what a process does when it
 * finds a version it does not know — is answered for the reference path in
 * `../migrate/runMigrations.ts` and nowhere else yet.
 *
 * `file_name` is snake_case here and camelCase in legacy, where TypeORM's default naming
 * leaked into one column of one table. gradido2 spells columns one way.
 */
export const migrationsPg = pgTable('migrations', {
  version: pgInteger('version').primaryKey(),
  fileName: varchar('file_name', { length: 256 }),
  date: pgTimestamp('date', { withTimezone: true, precision: 3, mode: 'date' }).notNull(),
})

export const migrationsSqlite = sqliteTable('migrations', {
  version: integer('version', { mode: 'number' }).primaryKey(),
  fileName: text('file_name'),
  date: integer('date', { mode: 'timestamp_ms' }).notNull(),
})
