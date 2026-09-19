#include "service_core/master_seed.h"

#include <sodium.h>
#include <string.h>
#include <uv.h>

#include "gradido_blockchain_core/crypto/sign.h"

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int sc_master_seed_parse(const char *text, uint8_t out[SC_MASTER_SEED_BYTES])
{
    size_t i;

    if (text == NULL || out == NULL || strlen(text) != 2 * SC_MASTER_SEED_BYTES)
        return 0;
    for (i = 0; i != SC_MASTER_SEED_BYTES; ++i) {
        const int high = hex_digit(text[2 * i]);
        const int low = hex_digit(text[2 * i + 1]);
        if (high < 0 || low < 0) {
            sodium_memzero(out, SC_MASTER_SEED_BYTES);
            return 0;
        }
        out[i] = (uint8_t)(high << 4 | low);
    }
    return 1;
}

void sc_master_seed_to_hex(const uint8_t seed[SC_MASTER_SEED_BYTES],
                           char out[SC_MASTER_SEED_HEX_SIZE])
{
    (void)sodium_bin2hex(out, SC_MASTER_SEED_HEX_SIZE, seed, SC_MASTER_SEED_BYTES);
}

static void put_be64(uint8_t *out, uint64_t value)
{
    int i;
    for (i = 7; i >= 0; --i) {
        out[i] = (uint8_t)value;
        value >>= 8;
    }
}

sc_status sc_master_seed_new(const char *typed, uint8_t out[SC_MASTER_SEED_BYTES])
{
    uint8_t random[32];
    uint8_t clocks[16];
    uv_timeval64_t now;
    crypto_generichash_state state;

    if (out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (sodium_init() < 0)
        return SC_ERR_UNAVAILABLE;

    randombytes_buf(random, sizeof(random));
    if (uv_gettimeofday(&now) != 0)
        memset(&now, 0, sizeof(now));
    put_be64(clocks, (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_usec / 1000u);
    put_be64(clocks + 8, uv_hrtime());

    /* The BLAKE2b-256 of grdc_hash_generic, which the reference path uses, fed in three parts. */
    (void)crypto_generichash_init(&state, NULL, 0, SC_MASTER_SEED_BYTES);
    (void)crypto_generichash_update(&state, random, sizeof(random));
    (void)crypto_generichash_update(&state, clocks, sizeof(clocks));
    if (typed != NULL)
        (void)crypto_generichash_update(&state, (const uint8_t *)typed, strlen(typed));
    (void)crypto_generichash_final(&state, out, SC_MASTER_SEED_BYTES);

    sodium_memzero(random, sizeof(random));
    sodium_memzero(&state, sizeof(state));
    return SC_OK;
}

sc_status sc_master_seed_derive_dht(const uint8_t seed[SC_MASTER_SEED_BYTES], uint8_t node_seed[32],
                                    uint8_t node_key[32])
{
    grdc_sign_key_pair root;
    grdc_sign_key_pair node;
    sc_status status = SC_OK;

    if (seed == NULL || node_seed == NULL || node_key == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    grdc_sign_key_pair_init(&root);
    grdc_sign_key_pair_init(&node);
    if (grdc_sign_key_pair_generate_from_seed(&root, seed, SC_MASTER_SEED_BYTES) != ARNM_SUCCESS ||
        grdc_sign_key_pair_derive(&node, &root, SC_MASTER_SEED_PATH_DHT) != ARNM_SUCCESS) {
        status = SC_ERR_MALFORMED;
    } else {
        memcpy(node_seed, node.seed, 32);
        memcpy(node_key, node.public_key, 32);
    }
    sodium_memzero(&root, sizeof(root));
    sodium_memzero(&node, sizeof(node));
    return status;
}
