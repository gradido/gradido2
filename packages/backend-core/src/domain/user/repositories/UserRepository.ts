import { and, eq, isNull } from 'drizzle-orm'
import {
  type DatabaseConnection,
  userContactsPg,
  userContactsSqlite,
  usersPg,
  usersSqlite,
} from '../../../database'
import type { AddressOwner, NewAccount } from '../user.data'

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
   * Whether this community already has a member with that gradido id.
   *
   * Scoped by community because `users_uuid_key` is, and asking table-wide would be a
   * different question: another community's member holding the same uuid is not a conflict,
   * it is what the two-column key exists to allow.
   *
   * It does not replace the index — between this select and the insert there is a window,
   * and only the index closes it. It is here because a *generated* value that turns out to
   * be taken can simply be drawn again; see `../gradidoId.logic.ts`.
   */
  public async gradidoIdExists(gradidoId: string, communityId: bigint): Promise<boolean> {
    if (this.db.kind === 'sqlite') {
      const row = this.db.drizzle
        .select({ id: usersSqlite.id })
        .from(usersSqlite)
        .where(
          and(
            eq(usersSqlite.gradidoId, gradidoId),
            eq(usersSqlite.communityId, Number(communityId)),
          ),
        )
        .limit(1)
        .get()
      return row !== undefined
    }

    const rows = await this.db.drizzle
      .select({ id: usersPg.id })
      .from(usersPg)
      .where(and(eq(usersPg.gradidoId, gradidoId), eq(usersPg.communityId, communityId)))
      .limit(1)
    return rows.length > 0
  }

  /**
   * Writes the member and their login address, or neither, and answers with `users.id`.
   *
   * Just the id: everything else about a new account is what the caller passed in, and a
   * repository that echoed it back would only invite somebody to read it as confirmation.
   *
   * Three statements, because the two rows point at each other: the member exists before the
   * contact can name them, and `users.email_id` can only be written once the contact has an
   * id. Inside one transaction, so an account without an address — which nothing could log
   * into and nothing would report — cannot survive a failure halfway through.
   */
  public async createAccount(account: NewAccount): Promise<bigint> {
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
          .returning({ id: userContactsSqlite.id })
          .get()
        tx.update(usersSqlite)
          .set({ emailId: contactRow.id })
          .where(eq(usersSqlite.id, created.id))
          .run()
        return created.id
      })
      return BigInt(id)
    }

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
        .returning({ id: userContactsPg.id })
      await tx.update(usersPg).set({ emailId: contactRow.id }).where(eq(usersPg.id, created.id))
      return created.id
    })
    return id
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
