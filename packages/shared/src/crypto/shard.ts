import { createHash } from 'node:crypto'

/**
 * Which shard a community is in, and the topic keys a node follows to keep its transactions --
 * DHT_SHARD_COUNT, DHT_SHARD_CONTEXT, DHT_SHARD_TOPIC_CONTEXT and DHT_COMMUNITY_TOPIC_CONTEXT in
 * contracts/const.json. `fast-servers/service-core/src/shard.c` is the other half, and
 * contracts/test-vectors/shard.json holds both to the same bytes.
 */

/** DHT_SHARD_COUNT. A power of two, at most 256. */
export const DHT_SHARD_COUNT = 64

const SHARD_CONTEXT = Buffer.from('gradido/shard/v1', 'utf8')
const SHARD_TOPIC_CONTEXT = Buffer.from('gradido/shard/v1/topic', 'utf8')
const COMMUNITY_TOPIC_CONTEXT = Buffer.from('gradido/community/v1', 'utf8')

const sha256 = (...parts: Uint8Array[]) =>
  new Uint8Array(createHash('sha256').update(Buffer.concat(parts)).digest())

/** sha256(DHT_SHARD_CONTEXT || community key)[0], masked to DHT_SHARD_COUNT. */
export function shardOf(communityKey: Uint8Array): number {
  if (communityKey.length !== 32) {
    throw new Error(`a community key is 32 bytes, got ${communityKey.length}`)
  }
  return (sha256(SHARD_CONTEXT, communityKey)[0] as number) & (DHT_SHARD_COUNT - 1)
}

/** sha256(DHT_SHARD_TOPIC_CONTEXT || shard byte): the topic a shard's communities publish to. */
export function shardTopicKey(shard: number): Uint8Array {
  if (!Number.isInteger(shard) || shard < 0 || shard >= DHT_SHARD_COUNT) {
    throw new Error(`a shard is 0 to ${DHT_SHARD_COUNT - 1}, got ${shard}`)
  }
  return sha256(SHARD_TOPIC_CONTEXT, Uint8Array.of(shard))
}

/** sha256(DHT_COMMUNITY_TOPIC_CONTEXT || community key): the community's own topic. */
export function communityTopicKey(communityKey: Uint8Array): Uint8Array {
  if (communityKey.length !== 32) {
    throw new Error(`a community key is 32 bytes, got ${communityKey.length}`)
  }
  return sha256(COMMUNITY_TOPIC_CONTEXT, communityKey)
}
