import { afterEach, beforeEach, describe, expect, mock, test } from 'bun:test'
import { Logger } from '@gradido/service-core'
import type { UserCreateRequest } from '@gradido/shared/schemas'
import type { BackendContext } from '../../../BackendContext'
import { openTestDatabase, type TestDatabase, testDatabaseKinds, testQuery } from '../../../testing'
import { createHomeCommunity } from '../../community'
import { newEmailVerificationCode } from '../logic/verificationCode.logic'
import { registerAccount } from './register-account'

/**
 * Fixed verification codes for the duration of one test, restored afterwards.
 *
 * `mock.module` replaces the module for the whole process and stays replaced — bun has no
 * scope to undo it in, so the undo is another `mock.module` putting the real function back.
 * Without it this leaks into `verificationCode.logic.test.ts`, which draws a thousand codes
 * and asserts on their spread: it would then be asserting on this array. That is not
 * hypothetical, it is what happened, and it is why the restore runs from `finally`.
 */
const PATH = '../logic/verificationCode.logic'

/* Copied at import time, before anything has mocked it. The imported name itself is a live
   binding that `mock.module` repoints, so restoring *through* it would put the mock back. */
const realCode = newEmailVerificationCode

async function withCodes(codes: bigint[], body: () => Promise<void>): Promise<void> {
  let at = 0
  mock.module(PATH, () => ({
    /* The last one repeats, so a test says only as many codes as it cares about. */
    newEmailVerificationCode: () => codes[Math.min(at++, codes.length - 1)],
  }))
  try {
    await body()
  } finally {
    mock.module(PATH, () => ({ newEmailVerificationCode: realCode }))
  }
}

const silent = Logger.create({ LOG_LEVEL: 'fatal', LOG_FILE: '', NODE_ENV: 'test' })

const request = (overrides: Partial<UserCreateRequest> = {}): UserCreateRequest => ({
  firstName: 'Einhorn',
  lastName: 'Immond',
  email: 'einhorn@gradido.net',
  language: 'de',
  ...overrides,
})

for (const kind of testDatabaseKinds()) {
  describe(`registerAccount (${kind})`, () => {
    let database: TestDatabase
    let context: BackendContext

    beforeEach(async () => {
      database = await openTestDatabase(kind)
      /* Registration writes users.community_id, so a member needs a community to belong to
         — the same one the running backend sets up on its first start. */
      const homeCommunity = await createHomeCommunity(
        { db: database.connection, logger: silent },
        { name: 'Gradido Test', description: null, url: 'https://gdd.example.org' },
      )
      context = { db: database.connection, logger: silent, homeCommunity }
    })

    afterEach(async () => {
      await database.close()
    })

    const users = () => testQuery(database.connection, 'SELECT * FROM users')
    const contacts = () => testQuery(database.connection, 'SELECT * FROM user_contacts')

    test('writes one member and one contact, pointing at each other', async () => {
      await registerAccount(context, request())

      const [user] = await users()
      const [contact] = await contacts()
      expect(String(user.gradido_id)).toMatch(
        /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/,
      )
      expect(String(user.community_id)).toBe(String(context.homeCommunity.id))
      expect(user.first_name).toBe('Einhorn')
      expect(user.last_name).toBe('Immond')
      expect(user.language).toBe('de')
      expect(contact.email).toBe('einhorn@gradido.net')
      // The link exists in both directions, which is the part a failed transaction breaks.
      expect(String(contact.user_id)).toBe(String(user.id))
      expect(String(user.email_id)).toBe(String(contact.id))
    })

    test('starts the account without a password and without a confirmed address', async () => {
      await registerAccount(context, request())

      const [user] = await users()
      const [contact] = await contacts()
      expect(user.password_hash).toBeNull()
      expect(Number(user.password_encryption_type)).toBe(0)
      // NO_PASSWORD and an unchecked address: the member cannot sign in until they have
      // followed the activation mail, which is the point of the whole flow.
      expect(Boolean(contact.email_checked)).toBe(false)
      // OptInType.EMAIL_OPT_IN_REGISTER
      expect(Number(contact.email_opt_in_type_id)).toBe(1)
      expect(contact.type).toBe('EMAIL')
      expect(Number(contact.email_resend_count)).toBe(0)
    })

    test('gives the contact a verification code inside the contracted range', async () => {
      await registerAccount(context, request())

      const [contact] = await contacts()
      const code = BigInt(String(contact.email_verification_code))
      expect(code).toBeGreaterThan(0n)
      // 2^53-1: wider than this and SQLite would hand it back rounded.
      expect(code).toBeLessThanOrEqual(9007199254740991n)
    })

    test('both rows carry the same moment of creation', async () => {
      await registerAccount(context, request())

      const [user] = await users()
      const [contact] = await contacts()
      expect(new Date(user.created_at as string | number).getTime()).toBe(
        new Date(contact.created_at as string | number).getTime(),
      )
    })

    /* Trimming and lowercasing are not tested here any more, because they do not happen here
       any more: `emailSchema` does both in its pipe, so an address is one string before it
       reaches this layer. `packages/shared/src/schemas/auth.test.ts` is where that is checked,
       and this Interaction is entitled to assume it -- which is also what lets
       `user_contacts_email_key` be the authority on whether an address is taken. */

    // The silence rule. This is the test that would notice the day somebody "helpfully"
    // reports the duplicate — which is what turns registration into a membership oracle.
    describe('when the address is already registered', () => {
      test('writes nothing at all', async () => {
        await registerAccount(context, request())
        await registerAccount(context, request({ firstName: 'Someone', lastName: 'Else' }))

        expect(await users()).toHaveLength(1)
        expect(await contacts()).toHaveLength(1)
      })

      // The route has no body to differ in, so what is left to get wrong is throwing: the
      // unique index on user_contacts.email is right there, and letting it fire would turn
      // registration into a membership oracle for anyone with a list of addresses.
      test('succeeds exactly as a new address does', async () => {
        await registerAccount(context, request())

        expect(
          registerAccount(context, request({ firstName: 'Someone', lastName: 'Else' })),
        ).resolves.toBeUndefined()
      })

      test('leaves the member who owns the address untouched', async () => {
        await registerAccount(context, request())
        const before = (await users())[0]

        await registerAccount(context, request({ firstName: 'Someone', lastName: 'Else' }))

        expect((await users())[0]).toEqual(before)
      })

      test('draws again when a generated value collides, and says so', async () => {
        /* The path nothing else reaches: both generated values are answered by an index at the
           moment of the write, so the only way to see the retry is to make a draw repeat. The
           code is fixed once, then freed -- so the second registration collides on
           `user_contacts_email_verification_code_key`, draws again, and gets through.

           Without this test the retry is code that never runs, which is the same as code that
           does not work. */
        await withCodes([4242n, 4242n, 9999n], async () => {
          await registerAccount(context, request({ email: 'erste@gradido.net' }))
          await registerAccount(context, request({ email: 'zweite@gradido.net' }))

          const written = await contacts()
          expect(written).toHaveLength(2)
          /* The second one carries the redrawn value, not the one that collided. */
          expect(written.map((row) => String(row.email_verification_code)).sort()).toEqual([
            '4242',
            '9999',
          ])
        })
      })

      test('lets the unique index decide, not a lookup before it', async () => {
        /* The property that replaced the pre-check: nothing reads the address before writing
           it, so what makes a second registration silent is `user_contacts_email_key`. This
           writes the contact row by hand first, so the Interaction meets an address it never
           looked up -- and still answers as if the registration were new. */
        await registerAccount(context, request())
        const before = await users()

        await registerAccount(context, request({ firstName: 'Zweiter', lastName: 'Versuch' }))

        expect(await users()).toHaveLength(before.length)
        expect(await contacts()).toHaveLength(1)
      })
    })
  })
}
