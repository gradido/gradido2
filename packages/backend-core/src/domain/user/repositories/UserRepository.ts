import { and, eq, isNull } from 'drizzle-orm'
import {
  type DatabaseConnection,
  userContactsPg,
  userContactsSqlite,
  usersPg,
  usersSqlite,
} from '../../../database'
import type { AddressOwner, CreateAccountResult, NewAccount } from '../user.data'

/**
 * How the transaction says "the address is taken" on its way out.
 *
 * A throw and not a return, because it has to roll back: by the time the conflict is known, a
 * member row has already been written, and drizzle rolls a transaction back when its callback
 * throws and commits when it returns. So the only way to leave without keeping that row is to
 * throw — which is why this is not an Error anybody reports. It never leaves createAccount.
 */
/**
 * Whether this error is a unique violation, and on which constraint.
 *
 * Two drivers, two ways of saying it, and both were read off the drivers rather than guessed:
 * bun's PostgreSQL client puts the SQLSTATE in `errno` — 23505 is unique_violation, 23503 a
 * foreign key, 23502 a not-null — and the constraint's name in `constraint`. bun:sqlite has no
 * constraint name to give and says `SQLITE_CONSTRAINT_UNIQUE` in `code`, with the columns in a
 * sentence; the sentence is used for the log and never for a decision.
 *
 * Answers undefined for anything else, which is what keeps a foreign key or a dead connection
 * from being retried as though a coin had landed badly.
 */
function uniqueViolationOf(error: unknown): string | undefined {
  const e = error as { errno?: unknown; code?: unknown; constraint?: unknown; message?: unknown }
  if (String(e.errno) === '23505') {
    return typeof e.constraint === 'string' ? e.constraint : 'unique'
  }
  if (e.code === 'SQLITE_CONSTRAINT_UNIQUE') {
    const said = String(e.message ?? '')
    const at = said.indexOf('UNIQUE constraint failed: ')
    return at === -1 ? 'unique' : said.slice(at + 'UNIQUE constraint failed: '.length).trim()
  }
  return undefined
}

/** How deep a wrapped driver error is looked for before giving up. */
const CAUSE_DEPTH_MAX = 4

/**
 * The same question, asked of the error and of whatever it wraps.
 *
 * drizzle's PostgreSQL session catches the driver's rejection and rethrows it as a
 * `DrizzleQueryError` carrying the statement and its parameters, with the original on `cause`;
 * its SQLite session hands the driver's error straight out. So the two dialects differ not only
 * in what a unique violation *says* but in how deep it is — and a check that only looked at the
 * top would silently stop recognising collisions on PostgreSQL, which is exactly how this was
 * found: the SQLite half of the test passed while the PostgreSQL half let the error escape.
 *
 * Bounded rather than a `while`, because a cause chain is data from a library and a cycle in it
 * would hang the request rather than fail it.
 */
function uniqueViolation(error: unknown): string | undefined {
  let at: unknown = error
  for (let depth = 0; depth < CAUSE_DEPTH_MAX && at !== null && at !== undefined; depth++) {
    const constraint = uniqueViolationOf(at)
    if (constraint !== undefined) {
      return constraint
    }
    at = (at as { cause?: unknown }).cause
  }
  return undefined
}

class AddressTaken extends Error {
  public constructor(public readonly takenBy: bigint) {
    super('the address is already registered')
    this.name = 'AddressTaken'
  }
}

/**
 * How an account is loaded and persisted. The Interaction decides *when*.
 *
 * Every method here branches on the dialect, and that is the design rather than a wart:
 * `connect.ts` hands out a discriminated union precisely so that a repository has to say
 * which database it is talking to. The row is built once, above the branch; what differs
 * below it is only the table object and whether the driver is awaited.
 *
 * Two things the branches do not share, and both are why an abstraction over them would be
 * a lie:
 *
 * ```text
 * ids           PostgreSQL returns bigint, SQLite a double. Widened here, once, so the
 *               domain never has to know which database an id came from.
 * transactions  Bun's PostgreSQL driver is a pool and its transaction is async; Bun's
 *               SQLite driver is one file handle and its transaction callback is
 *               synchronous. The same code cannot be both.
 * ```
 */
export class UserRepository {
  public constructor(private readonly db: DatabaseConnection) {}

  /**
   * Who holds this address, if anybody.
   *
   * Deleted members are excluded: a soft-deleted row still occupies the unique index on
   * `user_contacts.email`, so this answers "can this address be registered" correctly only
   * for the living. Reviving a deleted account is a different operation and does not exist
   * yet — until it does, an address belonging to a deleted member is unusable, which is what
   * legacy does too.
   */
  public async findAddressOwner(email: string): Promise<AddressOwner | undefined> {
    if (this.db.kind === 'sqlite') {
      const row = this.db.drizzle
        .select({
          id: usersSqlite.id,
          firstName: usersSqlite.firstName,
          lastName: usersSqlite.lastName,
          language: usersSqlite.language,
        })
        .from(userContactsSqlite)
        .innerJoin(usersSqlite, eq(usersSqlite.id, userContactsSqlite.userId))
        .where(and(eq(userContactsSqlite.email, email), isNull(usersSqlite.deletedAt)))
        .limit(1)
        .get()
      return row === undefined ? undefined : toAddressOwner(row)
    }

    const rows = await this.db.drizzle
      .select({
        id: usersPg.id,
        firstName: usersPg.firstName,
        lastName: usersPg.lastName,
        language: usersPg.language,
      })
      .from(userContactsPg)
      .innerJoin(usersPg, eq(usersPg.id, userContactsPg.userId))
      .where(and(eq(userContactsPg.email, email), isNull(usersPg.deletedAt)))
      .limit(1)
    const row = rows[0]
    return row === undefined ? undefined : toAddressOwner(row)
  }

  /**
   * Writes the member and their login address, or neither — and says which happened.
   *
   * **The unique index decides, not a lookup before it.** `user_contacts_email_key` is what
   * makes an address one member's, so this asks it by writing rather than asking a `SELECT`
   * first and then writing. The difference is not the saved round trip: a lookup answers about
   * a moment that is over by the time the insert runs, and two registrations for one free
   * address would both find it free — one would then fail on the index, which is a 500 that
   * happens only for addresses that were *not* yet registered. That is the membership oracle
   * `contracts/server/backend/user.json` closes by answering both cases with the same empty
   * 204, and no rule outside the database can keep it closed.
   *
   * `ON CONFLICT (email) DO NOTHING` rather than letting the insert fail, because three unique
   * constraints can refuse this row and they mean different things: the address being taken is
   * the contracted silence, while a collision on `email_verification_code` or on
   * `users_uuid_key` is a coincidence to be retried with fresh values. No row returned says
   * "the address" and nothing else, without reading an error message — which on SQLite would be
   * parsing English.
   *
   * Three statements, because the two rows point at each other: the member exists before the
   * contact can name them, and `users.email_id` — which of several addresses is the one mail
   * goes to — can only be written once the contact has an id. One transaction, so an account
   * without an address cannot survive a failure halfway through, and so the member row written
   * before a conflict is discovered goes away again.
   */
  public async createAccount(account: NewAccount): Promise<CreateAccountResult> {
    const user = {
      gradidoId: account.gradidoId,
      firstName: account.firstName,
      lastName: account.lastName,
      language: account.language,
      createdAt: account.createdAt,
    }
    const contact = {
      /* 'EMAIL' — contracts/types/UserContactType.json. */
      type: 'EMAIL',
      email: account.email,
      emailChecked: false,
      /* 1, EMAIL_OPT_IN_REGISTER — contracts/types/OptInType.json. */
      emailOptInTypeId: 1,
      createdAt: account.createdAt,
    }
    if (this.db.kind === 'sqlite') {
      try {
        const id = this.db.drizzle.transaction((tx) => {
          const created = tx
            .insert(usersSqlite)
            /* SQLite hands INTEGER to JavaScript as a double; row ids stay far below 2^53. */
            .values({ ...user, communityId: Number(account.communityId) })
            .returning({ id: usersSqlite.id })
            .get()
          const contactRow = tx
            .insert(userContactsSqlite)
            .values({
              ...contact,
              userId: created.id,
              /* SQLite stores it in a signed 64 bit INTEGER and hands it back as a double.
                 The contract bounds the code to 2^53-1 for exactly that reason, so this
                 narrowing is lossless — see verificationCode.logic.ts. */
              emailVerificationCode: Number(account.emailVerificationCode),
            })
            .onConflictDoNothing({ target: userContactsSqlite.email })
            .returning({ id: userContactsSqlite.id })
            .get()
          if (contactRow === undefined) {
            /* Who holds it, for the contracted `usr` on user.registration.denied. Read inside
               the transaction because DO NOTHING is not a failure — nothing is broken here,
               there is simply a member already. */
            const owner = tx
              .select({ id: userContactsSqlite.userId })
              .from(userContactsSqlite)
              .where(eq(userContactsSqlite.email, account.email))
              .get()
            throw new AddressTaken(BigInt(owner?.id ?? 0))
          }
          tx.update(usersSqlite)
            .set({ emailId: contactRow.id })
            .where(eq(usersSqlite.id, created.id))
            .run()
          return created.id
        })
        return { created: BigInt(id) }
      } catch (error) {
        /* The one way out of a transaction that has to roll back and is not a failure. The
           member row written a moment ago goes with it; the identity sequence does not, which
           is a gap in users.id and nothing more. */
        if (error instanceof AddressTaken) {
          return { takenBy: error.takenBy }
        }
        /* The address is answered above and never lands here, so a unique violation at this
           point is one of the two generated values having been drawn before. */
        const constraint = uniqueViolation(error)
        if (constraint !== undefined) {
          return { collided: constraint }
        }
        throw error
      }
    }

    try {
      const id = await this.db.drizzle.transaction(async (tx) => {
        const [created] = await tx
          .insert(usersPg)
          .values({ ...user, communityId: account.communityId })
          .returning({ id: usersPg.id })
        const [contactRow] = await tx
          .insert(userContactsPg)
          .values({
            ...contact,
            userId: created.id,
            emailVerificationCode: account.emailVerificationCode,
          })
          .onConflictDoNothing({ target: userContactsPg.email })
          .returning({ id: userContactsPg.id })
        if (contactRow === undefined) {
          const [owner] = await tx
            .select({ id: userContactsPg.userId })
            .from(userContactsPg)
            .where(eq(userContactsPg.email, account.email))
            .limit(1)
          throw new AddressTaken(owner?.id ?? 0n)
        }
        await tx.update(usersPg).set({ emailId: contactRow.id }).where(eq(usersPg.id, created.id))
        return created.id
      })
      return { created: id }
    } catch (error) {
      if (error instanceof AddressTaken) {
        return { takenBy: error.takenBy }
      }
      const constraint = uniqueViolation(error)
      if (constraint !== undefined) {
        return { collided: constraint }
      }
      throw error
    }
  }
}

/** The one place a row id stops being whatever the driver made of it. */
function toAddressOwner(row: {
  id: bigint | number
  firstName: string | null
  lastName: string | null
  language: string
}): AddressOwner {
  return {
    id: BigInt(row.id),
    /* Both name columns are nullable in the contract; legacy has rows that use it. */
    firstName: row.firstName ?? '',
    lastName: row.lastName ?? '',
    language: row.language,
  }
}
