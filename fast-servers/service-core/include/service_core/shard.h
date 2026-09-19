/*
 * Which shard a community is in, and the topic keys a node follows to keep its transactions --
 * DHT_SHARD_COUNT and the three contexts in contracts/const.json. The reference is
 * `packages/shared/src/crypto/shard.ts`; contracts/test-vectors/shard.json holds both to the
 * same bytes.
 */
#ifndef SERVICE_CORE_SHARD_H
#define SERVICE_CORE_SHARD_H

#include <stdint.h>

/** DHT_SHARD_COUNT. A power of two, at most 256. */
#define SC_SHARD_COUNT 64u

/** sha256(DHT_SHARD_CONTEXT || community key)[0], masked to SC_SHARD_COUNT. */
uint32_t sc_shard_of(const uint8_t community_key[32]);

/** sha256(DHT_SHARD_TOPIC_CONTEXT || shard byte). @p shard is below SC_SHARD_COUNT. */
void sc_shard_topic_key(uint32_t shard, uint8_t out[32]);

/** sha256(DHT_COMMUNITY_TOPIC_CONTEXT || community key). */
void sc_community_topic_key(const uint8_t community_key[32], uint8_t out[32]);

#endif /* SERVICE_CORE_SHARD_H */
