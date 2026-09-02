import {
  bigint as pgBigint,
  boolean as pgBoolean,
  integer as pgInteger,
  pgTable,
  timestamp as pgTimestamp,
  varchar as pgVarchar,
} from 'drizzle-orm/pg-core'
import { integer, sqliteTable, text } from 'drizzle-orm/sqlite-core'

/**
 * `user_contacts`, once per dialect — `contracts/db/user_contacts.json`.
 *
 * The whole contracted table, unlike `users`: nothing in it is waiting on a decision.
 *
 * `email` is globally unique. That is what makes the silence rule in `register-account.ts`
 * necessary rather than merely polite — without it, a second registration for a known
 * address would fail on this constraint, and the failure would be the answer.
 *
 * `email_verification_code` is a secret: never logged, never in a response. Its range is
 * bounded to 2^53-1 by the contract, because SQLite hands an INTEGER to JavaScript as a
 * double and a wider value would come back changed without anything failing.
 */
export const userContactsPg = pgTable('user_contacts', {
  id: pgBigint('id', { mode: 'bigint' }).generatedAlwaysAsIdentity().primaryKey(),
  userId: pgBigint('user_id', { mode: 'bigint' }).notNull(),
  /** `UserContactType` — 'EMAIL' or 'PHONE'. */
  type: pgVarchar('type', { length: 100 }),
  email: pgVarchar('email', { length: 255 }).notNull(),
  emailChecked: pgBoolean('email_checked').notNull().default(false),
  emailVerificationCode: pgBigint('email_verification_code', { mode: 'bigint' }).notNull(),
  /** `OptInType`. 0 is not a member of the enum; it means no opt-in is pending. */
  emailOptInTypeId: pgInteger('email_opt_in_type_id').notNull().default(0),
  emailResendCount: pgInteger('email_resend_count').notNull().default(0),
  gmsPublishEmail: pgBoolean('gms_publish_email').notNull().default(false),
  phone: pgVarchar('phone', { length: 255 }),
  countryCode: pgVarchar('country_code', { length: 255 }),
  gmsPublishPhone: pgInteger('gms_publish_phone').notNull().default(0),
  createdAt: pgTimestamp('created_at', {
    withTimezone: true,
    precision: 3,
    mode: 'date',
  }).notNull(),
  /* Written by the application, never by the dialect: legacy leans on MariaDB's ON UPDATE
     CURRENT_TIMESTAMP, PostgreSQL would need a trigger and SQLite has nothing at all. Both
     implementations set it explicitly so the three databases cannot disagree. */
  updatedAt: pgTimestamp('updated_at', { withTimezone: true, precision: 3, mode: 'date' }),
  deletedAt: pgTimestamp('deleted_at', { withTimezone: true, precision: 3, mode: 'date' }),
})

export const userContactsSqlite = sqliteTable('user_contacts', {
  id: integer('id', { mode: 'number' }).primaryKey({ autoIncrement: true }),
  userId: integer('user_id', { mode: 'number' }).notNull(),
  type: text('type'),
  email: text('email').notNull(),
  emailChecked: integer('email_checked', { mode: 'boolean' }).notNull().default(false),
  emailVerificationCode: integer('email_verification_code', { mode: 'number' }).notNull(),
  emailOptInTypeId: integer('email_opt_in_type_id', { mode: 'number' }).notNull().default(0),
  emailResendCount: integer('email_resend_count', { mode: 'number' }).notNull().default(0),
  gmsPublishEmail: integer('gms_publish_email', { mode: 'boolean' }).notNull().default(false),
  phone: text('phone'),
  countryCode: text('country_code'),
  gmsPublishPhone: integer('gms_publish_phone', { mode: 'number' }).notNull().default(0),
  createdAt: integer('created_at', { mode: 'timestamp_ms' }).notNull(),
  updatedAt: integer('updated_at', { mode: 'timestamp_ms' }),
  deletedAt: integer('deleted_at', { mode: 'timestamp_ms' }),
})
