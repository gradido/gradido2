import { describe, expect, test } from 'bun:test'
import {
  deriveDhtNodeKeyPair,
  MASTER_SEED_BYTES,
  MASTER_SEED_PATH_DHT,
  parseMasterSeed,
  signDhtDelegation,
} from '@gradido/shared/crypto'
import { loadMasterSeedVectors } from './master-seed.vectors.ts'

/**
 * `contracts/test-vectors/master-seed.json`, run against the TypeScript path.
 *
 * The other half is `fast-servers/tests/contract/test_master_seed_contract.cpp`. The derivation is
 * the same C function underneath both, so what this pins is the binding, the index and the byte
 * layout around it; the delegation is signed by `node:crypto` here and by libsodium there, and the
 * file's values were signed by libp2p-ffi itself.
 */

const { pathDht, seedBytes, vectors } = loadMasterSeedVectors()
const bytes = (hex: string) => new Uint8Array(Buffer.from(hex, 'hex'))
const hex = (value: Uint8Array) => Buffer.from(value).toString('hex')

describe('contract vectors: master-seed', () => {
  test('the constants are the ones the file was written against', () => {
    expect(MASTER_SEED_PATH_DHT).toBe(pathDht)
    expect(MASTER_SEED_BYTES).toBe(seedBytes)
  })

  for (const vector of vectors) {
    test(vector.id, () => {
      switch (vector.kind) {
        case 'parse':
          expect(parseMasterSeed(vector.text) !== undefined).toBe(vector.expect.accepted)
          break
        case 'derive': {
          const node = deriveDhtNodeKeyPair(bytes(vector.masterSeed))
          expect(hex(node.seed)).toBe(vector.expect.dhtNodeSeed)
          expect(node.publicKeyString).toBe(vector.expect.dhtNodeKey)
          break
        }
        case 'delegate': {
          const node = deriveDhtNodeKeyPair(bytes(vector.masterSeed))
          expect(node.publicKeyString).toBe(vector.expect.dhtNodeKey)
          const privateKey = bytes(vector.communitySeed + vector.expect.communityKey)
          const delegation = signDhtDelegation(privateKey, node.publicKey, BigInt(vector.expiresMs))
          expect(hex(delegation)).toBe(vector.expect.delegation)
          break
        }
      }
    })
  }
})
