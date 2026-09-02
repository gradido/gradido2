import {
  bigint as pgBigint,
  boolean as pgBoolean,
  integer as pgInteger,
  pgTable,
  timestamp as pgTimestamp,
  uuid as pgUuid,
  varchar as pgVarchar,
} from 'drizzle-orm/pg-core'
import { integer, sqliteTable, text } from 'drizzle-orm/sqlite-core'

/**
 * `users`, once per dialect — `contracts/db/users.json`.
 *
 * **Two definitions rather than one abstracted definition.** The dialects genuinely differ:
 * a uuid is a `uuid` column in PostgreSQL and text in SQLite, an instant is a
 * `timestamptz(3)` there and a signed integer here. A layer that hid that would have to
 * decide, for every column, which database it is pretending to be — and `connect.ts` already
 * made the opposite decision on purpose: the connection is a discriminated union so that a
 * repository has to say which database it is talking to.
 *
 * **What is here is what an account needs to exist, not the whole contracted table.** The
 * columns whose feature has not arrived yet are named in the migration that will add them;
 * see `../migrate/migrations/0001_users.ts`. The contract stays the authority on the full
 * shape — this is a subset of it, never a disagreement with it.
 *
 * **`communityId` is a row id, not the community's uuid, and it does not become one.** It
 * invites being "fixed" towards legacy, which carries `users.community_uuid`; carrying a
 * uuid in every member row would put 16 bytes of key into every index entry and every join
 * for the sake of a handful of communities. The uuid is the community's public,
 * federation-facing identity and stays in `communities.community_uuid`. The migration and
 * `contracts/db/users.json` both say so.
 *
 * The DDL in that migration is what the database is built from; these are the typed view of
 * it. Two spellings of one table drift, so `tables.test.ts` writes a row and reads it back on
 * both dialects — that round trip is what notices.
 */
export const usersPg = pgTable('users', {
  id: pgBigint('id', { mode: 'bigint' }).generatedAlwaysAsIdentity().primaryKey(),
  /** A member of another community, known here only by reference. */
  remote: pgBoolean('remote').notNull().default(false),
  /** This member's public identity — a uuid, and the one that does leave the database. */
  gradidoId: pgUuid('gradido_id').notNull(),
  /** `communities.id`. Unique keys on this table are per community, never table-wide. */
  communityId: pgBigint('community_id', { mode: 'bigint' }).notNull(),
  alias: pgVarchar('alias', { length: 20 }),
  /** `user_contacts.id` of the row holding the login address. Written after the contact. */
  emailId: pgBigint('email_id', { mode: 'bigint' }),
  firstName: pgVarchar('first_name', { length: 255 }),
  lastName: pgVarchar('last_name', { length: 255 }),
  language: pgVarchar('language', { length: 4 }).notNull().default('de'),
  /** argon2id in PHC string format. Null until the member sets a password. */
  passwordHash: pgVarchar('password_hash', { length: 255 }),
  passwordEncryptionType: pgInteger('password_encryption_type').notNull().default(0),
  createdAt: pgTimestamp('created_at', {
    withTimezone: true,
    precision: 3,
    mode: 'date',
  }).notNull(),
  /** Soft delete. Every query must exclude non-null unless it deliberately includes them. */
  deletedAt: pgTimestamp('deleted_at', { withTimezone: true, precision: 3, mode: 'date' }),
})

export const usersSqlite = sqliteTable('users', {
  /* SQLite hands an INTEGER to JavaScript as a double, so a row id arrives as a number and
     is widened to bigint at the repository boundary — the contract says identifiers are
     uint64 and the domain must not have to know which database it came from. */
  id: integer('id', { mode: 'number' }).primaryKey({ autoIncrement: true }),
  remote: integer('remote', { mode: 'boolean' }).notNull().default(false),
  gradidoId: text('gradido_id').notNull(),
  communityId: integer('community_id', { mode: 'number' }).notNull(),
  alias: text('alias'),
  emailId: integer('email_id', { mode: 'number' }),
  firstName: text('first_name'),
  lastName: text('last_name'),
  language: text('language').notNull().default('de'),
  passwordHash: text('password_hash'),
  passwordEncryptionType: integer('password_encryption_type', { mode: 'number' })
    .notNull()
    .default(0),
  createdAt: integer('created_at', { mode: 'timestamp_ms' }).notNull(),
  deletedAt: integer('deleted_at', { mode: 'timestamp_ms' }),
})
