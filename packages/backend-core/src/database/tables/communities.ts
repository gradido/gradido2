import {
  bigint as pgBigint,
  boolean as pgBoolean,
  bytea as pgBytea,
  pgTable,
  timestamp as pgTimestamp,
  uuid as pgUuid,
  varchar as pgVarchar,
} from 'drizzle-orm/pg-core'
import { blob, integer, sqliteTable, text } from 'drizzle-orm/sqlite-core'

/**
 * `communities`, once per dialect — `contracts/db/communities.json`.
 *
 * Every community this instance knows about, its own included. The home community is the
 * one row with `remote = false`, and there is exactly one; everything else is known by
 * reference and arrives through federation, which does not exist yet.
 *
 * A subset of the contracted table, on the same terms as `users`: what a community needs in
 * order to *be* one. `authenticated_at`, the GMS and JWT keys, `hiero_topic_id` and
 * `location` arrive with the features that read them — see the migration.
 *
 * The keys are `bytea` and `BLOB`, which is what the contract's `bytes(n)` means, and both
 * dialects hand them back as a Buffer. Never `text` or `varchar`: the first would store
 * `\x6b65...` and give a C reader a string where a key belongs, the second would apply a
 * collation to a key. `tables.test.ts` notices a key that comes back as anything else.
 *
 * **`private_key` is a secret and must never leave this file's neighbourhood.** It is not in
 * the row shape the application carries around (`community.data.ts`), it is not selected by
 * any query that feeds a response, and `contracts/logging.json` forbids logging it. It is
 * declared here because the column exists and the setup writes it, not because anything else
 * may read it.
 */
export const communitiesPg = pgTable('communities', {
  id: pgBigint('id', { mode: 'bigint' }).generatedAlwaysAsIdentity().primaryKey(),
  /** False on exactly one row: this instance's own community. */
  remote: pgBoolean('remote').notNull().default(true),
  /** Base URL. What a federation partner calls; unique. */
  url: pgVarchar('url', { length: 255 }).notNull(),
  /** ed25519 public key, 32 raw bytes. */
  publicKey: pgBytea('public_key').notNull(),
  /** ed25519 secret key, 64 raw bytes. Only the home community has one. Secret. */
  privateKey: pgBytea('private_key'),
  /** The community's public identity, and the only place a community uuid belongs. */
  communityUuid: pgUuid('community_uuid').notNull(),
  name: pgVarchar('name', { length: 40 }),
  description: pgVarchar('description', { length: 255 }),
  /** When the community was founded — `created_at` is when this row was written. */
  creationDate: pgTimestamp('creation_date', { withTimezone: true, precision: 3, mode: 'date' }),
  createdAt: pgTimestamp('created_at', {
    withTimezone: true,
    precision: 3,
    mode: 'date',
  }).notNull(),
  updatedAt: pgTimestamp('updated_at', { withTimezone: true, precision: 3, mode: 'date' }),
})

export const communitiesSqlite = sqliteTable('communities', {
  id: integer('id', { mode: 'number' }).primaryKey({ autoIncrement: true }),
  remote: integer('remote', { mode: 'boolean' }).notNull().default(true),
  url: text('url').notNull(),
  publicKey: blob('public_key', { mode: 'buffer' }).notNull(),
  privateKey: blob('private_key', { mode: 'buffer' }),
  communityUuid: text('community_uuid').notNull(),
  name: text('name'),
  description: text('description'),
  creationDate: integer('creation_date', { mode: 'timestamp_ms' }),
  createdAt: integer('created_at', { mode: 'timestamp_ms' }).notNull(),
  updatedAt: integer('updated_at', { mode: 'timestamp_ms' }),
})
