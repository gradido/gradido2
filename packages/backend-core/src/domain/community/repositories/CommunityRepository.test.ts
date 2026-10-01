import { afterEach, beforeEach, describe, expect, test } from 'bun:test'
import { openTestDatabase, type TestDatabase, testDatabaseKinds, testQuery } from '../../../testing'
import type { HomeCommunityInsertInput } from '../community.schema'

const communityUuid = '5f2c1d0e-8a4b-4c3d-9e6f-0a1b2c3d4e5f'
const publicKey = new Uint8Array(32).fill(1)
const createdAt = new Date('2026-10-01T12:00:00.000Z')
const privateKey = new Uint8Array(64).fill(2)

const community: HomeCommunityInsertInput = {
  url: 'https://gdd.example.org',
  name: 'Gradido Entwicklung',
  description: null,
  communityUuid,
  publicKey,
  privateKey,
  createdAt,
}

for (const kind of testDatabaseKinds()) {
  describe(`CommunityRepository (${kind})`, () => {
    let database: TestDatabase

    beforeEach(async () => {
      database = await openTestDatabase(kind)
    })

    afterEach(async () => {
      await database.close()
    })

    const communities = () => database.repositories.communities
    const rows = () => testQuery(database.connection, 'SELECT * FROM communities')

    test('reads a row back as the same value on either database', async () => {
      const id = await communities().createHomeCommunity(community)

      // The id is a bigint and the key a plain Uint8Array, whatever the driver handed out.
      expect(await communities().findHomeCommunity()).toEqual({
        id,
        communityUuid,
        url: community.url,
        name: community.name,
        description: null,
        publicKey,
      })
      expect(typeof id).toBe('bigint')
      expect(await communities().findHomeCommunitySigningKey()).toEqual(privateKey)
    })

    test('writes both instants as the one it was given', async () => {
      await communities().createHomeCommunity(community)

      const [row] = await rows()
      expect(new Date(row.created_at as number | Date)).toEqual(createdAt)
      expect(new Date(row.creation_date as number | Date)).toEqual(createdAt)
    })

    test('a transaction that returns keeps what was written in it', async () => {
      await database.connection.transaction(async (tx) => {
        await communities().createHomeCommunity(community, tx)
      })

      expect(await rows()).toHaveLength(1)
    })

    test('a transaction that throws leaves nothing behind', async () => {
      const failed = database.connection.transaction(async (tx) => {
        await communities().createHomeCommunity(community, tx)
        throw new Error('changed my mind')
      })

      await expect(failed).rejects.toThrow('changed my mind')
      expect(await rows()).toHaveLength(0)
    })

    test('a read inside the transaction sees what the transaction wrote', async () => {
      const found = await database.connection.transaction(async (tx) => {
        await communities().createHomeCommunity(community, tx)
        return communities().findHomeCommunity(tx)
      })

      expect(found?.url).toBe(community.url)
    })

    test('the database is usable again after a rollback', async () => {
      await database.connection
        .transaction(async (tx) => {
          await communities().createHomeCommunity(community, tx)
          throw new Error('changed my mind')
        })
        .catch(() => undefined)

      await communities().createHomeCommunity(community)
      expect(await rows()).toHaveLength(1)
    })

    test('refuses a home community without its private key', async () => {
      await communities().createHomeCommunity(community)
      await testQuery(database.connection, 'UPDATE communities SET private_key = NULL')

      await expect(communities().findHomeCommunitySigningKey()).rejects.toThrow('expect 64 bytes')
    })

    test('does not write a key of the wrong length', async () => {
      const shortKey = communities().createHomeCommunity({
        ...community,
        publicKey: new Uint8Array(31),
      })

      await expect(shortKey).rejects.toThrow('expect 32 bytes')
      expect(await rows()).toHaveLength(0)
    })

    test('refuses to read a public key that is not 32 bytes', async () => {
      await communities().createHomeCommunity(community)
      const oneByte = kind === 'sqlite' ? "x'00'" : "'\\x00'::bytea"
      await testQuery(database.connection, `UPDATE communities SET public_key = ${oneByte}`)

      await expect(communities().findHomeCommunity()).rejects.toThrow('expect 32 bytes')
    })
  })
}
