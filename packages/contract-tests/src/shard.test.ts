import { describe, expect, test } from 'bun:test'
import { communityTopicKey, DHT_SHARD_COUNT, shardOf, shardTopicKey } from '@gradido/shared/crypto'
import { loadShardVectors } from './shard.vectors.ts'

/**
 * `contracts/test-vectors/shard.json`, run against the TypeScript path. The other half is
 * `fast-servers/tests/contract/test_shard_contract.cpp`.
 */

const { shardCount, vectors } = loadShardVectors()
const bytes = (hex: string) => new Uint8Array(Buffer.from(hex, 'hex'))
const hex = (value: Uint8Array) => Buffer.from(value).toString('hex')

describe('contract vectors: shard', () => {
  test('the shard count is the one the file was written against', () => {
    expect(DHT_SHARD_COUNT).toBe(shardCount)
  })

  for (const vector of vectors) {
    test(vector.id, () => {
      if (vector.kind === 'community') {
        const shard = shardOf(bytes(vector.communityKey))
        expect(String(shard)).toBe(vector.expect.shard)
        expect(hex(shardTopicKey(shard))).toBe(vector.expect.shardTopicKey)
        expect(hex(communityTopicKey(bytes(vector.communityKey)))).toBe(
          vector.expect.communityTopicKey,
        )
      } else {
        expect(hex(shardTopicKey(Number(vector.shard)))).toBe(vector.expect.shardTopicKey)
      }
    })
  }
})
