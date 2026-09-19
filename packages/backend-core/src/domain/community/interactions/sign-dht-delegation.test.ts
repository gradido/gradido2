import { afterEach, beforeEach, describe, expect, test } from 'bun:test'
import { createPublicKey, verify } from 'node:crypto'
import { Logger } from '@gradido/service-core'
import { deriveDhtNodeKeyPair } from '@gradido/shared/crypto'
import type { DatabaseContext } from '../../../BackendContext'
import { openTestDatabase, type TestDatabase, testDatabaseKinds } from '../../../testing'
import { createHomeCommunity } from './create-home-community'
import { signDhtDelegationFor } from './sign-dht-delegation'

const silent = Logger.create({ LOG_LEVEL: 'fatal', LOG_FILE: '', NODE_ENV: 'test' })
const masterSeed = new Uint8Array(32).fill(7)

for (const kind of testDatabaseKinds()) {
  describe(`signDhtDelegationFor (${kind})`, () => {
    let database: TestDatabase
    let context: DatabaseContext

    beforeEach(async () => {
      database = await openTestDatabase(kind)
      context = { db: database.connection, logger: silent }
    })

    afterEach(async () => {
      await database.close()
    })

    test('the home community vouches for the node the master seed derives', async () => {
      const home = await createHomeCommunity(context, {
        name: 'Gradido Entwicklung',
        description: null,
        url: 'https://gdd.example.org',
      })
      const delegation = Buffer.from(await signDhtDelegationFor(context, masterSeed))

      expect(delegation.length).toBe(136)
      expect(delegation.subarray(0, 32)).toEqual(
        Buffer.from(deriveDhtNodeKeyPair(masterSeed).publicKey),
      )
      expect(delegation.subarray(32, 64)).toEqual(Buffer.from(home.publicKey))
      expect(delegation.readBigUInt64BE(64)).toBe(0n)

      // Signed by the key in the row, over what libp2p-ffi checks.
      const communityKey = createPublicKey({
        key: { kty: 'OKP', crv: 'Ed25519', x: Buffer.from(home.publicKey).toString('base64url') },
        format: 'jwk',
      })
      const signed = Buffer.concat([
        Buffer.from('libp2p-ffi delegation v1'),
        delegation.subarray(0, 72),
      ])
      expect(verify(null, signed, communityKey, delegation.subarray(72))).toBe(true)
    })

    test('signing twice gives the same bytes, so running setup again changes nothing', async () => {
      await createHomeCommunity(context, {
        name: 'Gradido',
        description: null,
        url: 'https://gdd.example.org',
      })
      expect(await signDhtDelegationFor(context, masterSeed)).toEqual(
        await signDhtDelegationFor(context, masterSeed),
      )
    })

    test('without a home community there is nothing to sign with', async () => {
      await expect(signDhtDelegationFor(context, masterSeed)).rejects.toThrow('no home community')
    })
  })
}
