#include "service_core/shard.h"

#include <sodium.h>
#include <string.h>

static void sha256_of(const char *context, const uint8_t *data, size_t size, uint8_t out[32])
{
    crypto_hash_sha256_state state;

    (void)sodium_init();
    (void)crypto_hash_sha256_init(&state);
    (void)crypto_hash_sha256_update(&state, (const uint8_t *)context, strlen(context));
    (void)crypto_hash_sha256_update(&state, data, size);
    (void)crypto_hash_sha256_final(&state, out);
}

uint32_t sc_shard_of(const uint8_t community_key[32])
{
    uint8_t digest[32];

    sha256_of("gradido/shard/v1", community_key, 32, digest);
    return digest[0] & (SC_SHARD_COUNT - 1u);
}

void sc_shard_topic_key(uint32_t shard, uint8_t out[32])
{
    const uint8_t byte = (uint8_t)shard;

    sha256_of("gradido/shard/v1/topic", &byte, 1, out);
}

void sc_community_topic_key(const uint8_t community_key[32], uint8_t out[32])
{
    sha256_of("gradido/community/v1", community_key, 32, out);
}
