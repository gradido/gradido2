import { afterEach, beforeEach, describe, expect, test } from 'bun:test'
import { Logger } from '@gradido/service-core'
import type { HomeCommunitySetup } from '@gradido/shared/schemas'
import type { DatabaseContext } from '../../../BackendContext'
import { openTestDatabase, type TestDatabase, testDatabaseKinds, testQuery } from '../../../testing'
import { CommunityRepository } from '../repositories'
import { createHomeCommunity } from './create-home-community'

const silent = Logger.create({ LOG_LEVEL: 'fatal', LOG_FILE: '', NODE_ENV: 'test' })

const setup: HomeCommunitySetup = {
  name: 'Gradido Entwicklung',
  description: 'Eine Testgemeinschaft',
  url: 'https://gdd.example.org',
}

for (const kind of testDatabaseKinds()) {
  describe(`createHomeCommunity (${kind})`, () => {
    let database: TestDatabase
    let context: DatabaseContext

    beforeEach(async () => {
      database = await openTestDatabase(kind)
      context = { db: database.connection, logger: silent }
    })

    afterEach(async () => {
      await database.close()
    })

    const communities = () => testQuery(database.connection, 'SELECT * FROM communities')

    test('a fresh database has no community, which is what makes the setup happen', async () => {
      expect(await new CommunityRepository(database.connection).findHomeCommunity()).toBeUndefined()
    })

    test('writes what the admin said and generates the rest', async () => {
      const home = await createHomeCommunity(context, setup)

      const [row] = await communities()
      expect(row.name).toBe(setup.name)
      expect(row.description).toBe(setup.description)
      expect(row.url).toBe(setup.url)
      // Generated, never asked for: a prompt for any of these is a way to get them wrong.
      expect(String(row.community_uuid)).toBe(home.communityUuid)
      expect(String(row.community_uuid)).toMatch(
        /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/,
      )
      expect(Boolean(row.remote)).toBe(false)
    })

    test('gives it an ed25519 key pair in the shape the columns hold', async () => {
      await createHomeCommunity(context, setup)

      const [row] = await communities()
      const publicKey = Buffer.from(row.public_key as Uint8Array)
      const privateKey = Buffer.from(row.private_key as Uint8Array)
      expect(publicKey).toHaveLength(32)
      expect(privateKey).toHaveLength(64)
      // libsodium's layout, which is what legacy stores and what the C path will read:
      // the 32-byte seed followed by the public key.
      expect(privateKey.subarray(32)).toEqual(publicKey)
    })

    // Holding the private key on a value every request handler can reach is how it ends up
    // in a log line. Whatever comes to need it loads it deliberately, at that moment.
    test('does not hand the private key back to the application', async () => {
      const home = await createHomeCommunity(context, setup)

      // The exact key set, not just the absence of one name: a field added here later is a
      // decision to hold something for the life of the process, and it should have to be made
      // on purpose.
      expect(Object.keys(home).sort()).toEqual([
        'communityUuid',
        'description',
        'id',
        'name',
        'publicKey',
        'url',
      ])
    })

    test('is found again by the repository, on a later start', async () => {
      const created = await createHomeCommunity(context, setup)

      const found = await new CommunityRepository(database.connection).findHomeCommunity()
      expect(found).toEqual(created)
    })

    test('refuses a second home community rather than picking one', async () => {
      await createHomeCommunity(context, setup)
      await createHomeCommunity(context, { ...setup, url: 'https://other.example.org' })

      expect(new CommunityRepository(database.connection).findHomeCommunity()).rejects.toThrow(
        /more than one home community/u,
      )
    })
  })
}
