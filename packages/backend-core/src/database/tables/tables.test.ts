import { afterEach, beforeEach, describe, expect, test } from 'bun:test'
import { eq } from 'drizzle-orm'
import { openTestDatabase, type TestDatabase, testDatabaseKinds } from '../../testing'
import { communitiesPg, communitiesSqlite } from './communities'
import { userContactsPg, userContactsSqlite } from './userContacts'
import { usersPg, usersSqlite } from './users'

/**
 * The table definitions and the migration DDL are two spellings of one schema, so they can
 * drift — a column added to one and not the other, a type that means something different in
 * the dialect it was copied into. Nothing about that fails at compile time.
 *
 * What notices is writing *every declared column* through drizzle and reading it back off a
 * database built by the migrations. A column the DDL does not have fails on the insert; a
 * column that means something else fails on the comparison.
 */
const CREATED = new Date('2026-03-01T12:34:56.789Z')
const UPDATED = new Date('2026-04-02T01:02:03.004Z')

const bytes = (length: number, seed: number) =>
  Buffer.from(Array.from({ length }, (_value, index) => (index * 7 + seed) % 256))

for (const kind of testDatabaseKinds()) {
  describe(`tables (${kind})`, () => {
    let database: TestDatabase

    beforeEach(async () => {
      database = await openTestDatabase(kind)
    })

    afterEach(async () => {
      await database.close()
    })

    /**
     * A community to hang members off, since `users.community_id` is NOT NULL.
     *
     * Every one gets its own public key, because `communities_public_key_key` says two
     * communities cannot share one — see the test below for why that matters.
     */
    let nextKey = 0
    const aCommunity = async (url = 'https://gdd.example.org'): Promise<bigint> => {
      nextKey += 1
      const row = {
        url,
        publicKey: bytes(32, nextKey),
        communityUuid: crypto.randomUUID(),
        createdAt: CREATED,
      }
      if (database.connection.kind === 'sqlite') {
        const [created] = database.connection.drizzle
          .insert(communitiesSqlite)
          .values(row)
          .returning({ id: communitiesSqlite.id })
          .all()
        return BigInt(created.id)
      }
      const [created] = await database.connection.drizzle
        .insert(communitiesPg)
        .values(row)
        .returning({ id: communitiesPg.id })
      return created.id
    }

    test('communities survives a round trip through every column it declares', async () => {
      const written = {
        remote: false,
        url: 'https://gdd.example.org',
        publicKey: bytes(32, 3),
        privateKey: bytes(64, 5),
        communityUuid: '9d2b1c44-8f31-4a52-9d6a-2c9a3c4b7e11',
        name: 'Gradido Entwicklung',
        description: 'Eine Testgemeinschaft',
        creationDate: CREATED,
        createdAt: CREATED,
        updatedAt: UPDATED,
      }

      const read =
        database.connection.kind === 'sqlite'
          ? database.connection.drizzle
              .insert(communitiesSqlite)
              .values(written)
              .returning()
              .all()[0]
          : (await database.connection.drizzle.insert(communitiesPg).values(written).returning())[0]

      // The keys are the point: a bytea or BLOB column that came back as a hex string
      // instead of bytes would be a signature nobody can verify.
      expect(Buffer.from(read.publicKey)).toEqual(written.publicKey)
      expect(read.privateKey === null ? null : Buffer.from(read.privateKey)).toEqual(
        written.privateKey,
      )
      expect({ ...read, id: undefined, publicKey: undefined, privateKey: undefined }).toEqual({
        ...written,
        id: undefined,
        publicKey: undefined,
        privateKey: undefined,
      })
    })

    test('users survives a round trip through every column it declares', async () => {
      const communityId = await aCommunity()
      const written = {
        remote: true,
        gradidoId: '3f2504e0-4f89-41d3-9a0c-0305e82c3301',
        alias: 'einhorn',
        firstName: 'Einhorn',
        lastName: 'Immond',
        language: 'de',
        passwordHash: '$argon2id$v=19$m=65536,t=3,p=4$c2FsdA$aGFzaA',
        passwordEncryptionType: 2,
        createdAt: CREATED,
        deletedAt: UPDATED,
      }

      const read =
        database.connection.kind === 'sqlite'
          ? database.connection.drizzle
              .insert(usersSqlite)
              .values({ ...written, communityId: Number(communityId) })
              .returning()
              .all()[0]
          : (
              await database.connection.drizzle
                .insert(usersPg)
                .values({ ...written, communityId })
                .returning()
            )[0]

      expect(String(read.communityId)).toBe(String(communityId))
      expect({ ...read, id: undefined, emailId: undefined, communityId: undefined }).toEqual({
        ...written,
        id: undefined,
        emailId: undefined,
        communityId: undefined,
      })
    })

    test('user_contacts survives a round trip through every column it declares', async () => {
      const communityId = await aCommunity()
      const contact = {
        type: 'EMAIL',
        email: 'einhorn@gradido.net',
        emailChecked: true,
        emailOptInTypeId: 1,
        emailResendCount: 3,
        gmsPublishEmail: true,
        phone: '+49 30 123456',
        countryCode: 'DE',
        gmsPublishPhone: 2,
        createdAt: CREATED,
        updatedAt: UPDATED,
        deletedAt: null,
      }

      if (database.connection.kind === 'sqlite') {
        const [user] = database.connection.drizzle
          .insert(usersSqlite)
          .values({
            gradidoId: crypto.randomUUID(),
            communityId: Number(communityId),
            createdAt: CREATED,
          })
          .returning({ id: usersSqlite.id })
          .all()
        const [read] = database.connection.drizzle
          .insert(userContactsSqlite)
          .values({ ...contact, userId: user.id, emailVerificationCode: 9007199254740991 })
          .returning()
          .all()
        expect({ ...read, id: undefined }).toEqual({
          ...contact,
          userId: user.id,
          /* The widest value the contract allows, written and read back unchanged — the
             whole reason the bound is where it is. */
          emailVerificationCode: 9007199254740991,
          id: undefined,
        })
        return
      }

      const [user] = await database.connection.drizzle
        .insert(usersPg)
        .values({ gradidoId: crypto.randomUUID(), communityId, createdAt: CREATED })
        .returning({ id: usersPg.id })
      const [read] = await database.connection.drizzle
        .insert(userContactsPg)
        .values({ ...contact, userId: user.id, emailVerificationCode: 9007199254740991n })
        .returning()
      expect({ ...read, id: undefined }).toEqual({
        ...contact,
        userId: user.id,
        emailVerificationCode: 9007199254740991n,
        id: undefined,
      })
    })

    // The two keys that are per community. Two members of *different* communities may hold
    // the same alias and, in principle, the same gradido id; two members of the same one
    // may not. A table-wide unique would pass the second half of this and fail the first.
    test('a gradido id and an alias are unique per community, not across them', async () => {
      const first = await aCommunity('https://one.example.org')
      const second = await aCommunity('https://two.example.org')
      const gradidoId = '3f2504e0-4f89-41d3-9a0c-0305e82c3399'

      const insertMember = async (communityId: bigint) => {
        if (database.connection.kind === 'sqlite') {
          database.connection.drizzle
            .insert(usersSqlite)
            .values({
              gradidoId,
              alias: 'einhorn',
              communityId: Number(communityId),
              createdAt: CREATED,
            })
            .run()
          return
        }
        await database.connection.drizzle
          .insert(usersPg)
          .values({ gradidoId, alias: 'einhorn', communityId, createdAt: CREATED })
      }

      await insertMember(first)
      await insertMember(second)
      expect(insertMember(first)).rejects.toThrow()
    })

    // The answer to the open question legacy left behind: its migration 0065 created this
    // constraint and its 0068 lost it while widening the column. The key is what a
    // federation handshake identifies a community by, so two rows holding one key is not a
    // duplicate — it is an ambiguity about who just authenticated.
    test('two communities cannot share a public key', async () => {
      const shared = bytes(32, 99)
      const insertCommunity = async (url: string) => {
        const row = {
          url,
          publicKey: shared,
          communityUuid: crypto.randomUUID(),
          createdAt: CREATED,
        }
        if (database.connection.kind === 'sqlite') {
          database.connection.drizzle.insert(communitiesSqlite).values(row).run()
          return
        }
        await database.connection.drizzle.insert(communitiesPg).values(row)
      }

      await insertCommunity('https://one.example.org')
      expect(insertCommunity('https://two.example.org')).rejects.toThrow()
    })

    test('an address can only be registered once, whatever the community', async () => {
      const first = await aCommunity('https://one.example.org')
      const second = await aCommunity('https://two.example.org')

      const insertContact = async (communityId: bigint, email: string) => {
        const code = Math.floor(Math.random() * 2 ** 40)
        if (database.connection.kind === 'sqlite') {
          const [user] = database.connection.drizzle
            .insert(usersSqlite)
            .values({
              gradidoId: crypto.randomUUID(),
              communityId: Number(communityId),
              createdAt: CREATED,
            })
            .returning({ id: usersSqlite.id })
            .all()
          database.connection.drizzle
            .insert(userContactsSqlite)
            .values({ userId: user.id, email, emailVerificationCode: code, createdAt: CREATED })
            .run()
          return
        }
        const [user] = await database.connection.drizzle
          .insert(usersPg)
          .values({ gradidoId: crypto.randomUUID(), communityId, createdAt: CREATED })
          .returning({ id: usersPg.id })
        await database.connection.drizzle.insert(userContactsPg).values({
          userId: user.id,
          email,
          emailVerificationCode: BigInt(code),
          createdAt: CREATED,
        })
      }

      await insertContact(first, 'einhorn@gradido.net')
      // Unlike the keys on users, this one is global on purpose: an address identifies a
      // person, and two communities cannot share it.
      expect(insertContact(second, 'einhorn@gradido.net')).rejects.toThrow()
    })

    test('the two account tables point at each other', async () => {
      const communityId = await aCommunity()

      if (database.connection.kind === 'sqlite') {
        const [user] = database.connection.drizzle
          .insert(usersSqlite)
          .values({
            gradidoId: crypto.randomUUID(),
            communityId: Number(communityId),
            createdAt: CREATED,
          })
          .returning({ id: usersSqlite.id })
          .all()
        const [contact] = database.connection.drizzle
          .insert(userContactsSqlite)
          .values({
            userId: user.id,
            email: 'link@gradido.net',
            emailVerificationCode: 4711,
            createdAt: CREATED,
          })
          .returning({ id: userContactsSqlite.id })
          .all()
        database.connection.drizzle
          .update(usersSqlite)
          .set({ emailId: contact.id })
          .where(eq(usersSqlite.id, user.id))
          .run()
        const [read] = database.connection.drizzle
          .select({ emailId: usersSqlite.emailId })
          .from(usersSqlite)
          .where(eq(usersSqlite.id, user.id))
          .all()
        expect(read.emailId).toBe(contact.id)
        return
      }

      const [user] = await database.connection.drizzle
        .insert(usersPg)
        .values({ gradidoId: crypto.randomUUID(), communityId, createdAt: CREATED })
        .returning({ id: usersPg.id })
      const [contact] = await database.connection.drizzle
        .insert(userContactsPg)
        .values({
          userId: user.id,
          email: 'link@gradido.net',
          emailVerificationCode: 4711n,
          createdAt: CREATED,
        })
        .returning({ id: userContactsPg.id })
      await database.connection.drizzle
        .update(usersPg)
        .set({ emailId: contact.id })
        .where(eq(usersPg.id, user.id))
      const [read] = await database.connection.drizzle
        .select({ emailId: usersPg.emailId })
        .from(usersPg)
        .where(eq(usersPg.id, user.id))
      expect(read.emailId).toBe(contact.id)
    })
  })
}
