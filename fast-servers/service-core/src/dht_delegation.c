#include "service_core/dht_delegation.h"

#include <sodium.h>
#include <string.h>

#define CONTEXT "libp2p-ffi delegation v1"
#define CONTEXT_BYTES (sizeof(CONTEXT) - 1)

sc_status sc_dht_delegation_sign(const uint8_t community_private_key[64],
                                 const uint8_t node_key[32], uint64_t expires_ms,
                                 uint8_t out[SC_DHT_DELEGATION_BYTES])
{
    uint8_t signed_part[CONTEXT_BYTES + 72];
    int i;

    if (community_private_key == NULL || node_key == NULL || out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (sodium_init() < 0)
        return SC_ERR_UNAVAILABLE;

    memcpy(out, node_key, 32);
    memcpy(out + 32, community_private_key + 32, 32);
    for (i = 7; i >= 0; --i) {
        out[64 + i] = (uint8_t)expires_ms;
        expires_ms >>= 8;
    }
    memcpy(signed_part, CONTEXT, CONTEXT_BYTES);
    memcpy(signed_part + CONTEXT_BYTES, out, 72);
    (void)crypto_sign_detached(out + 72, NULL, signed_part, sizeof(signed_part),
                               community_private_key);
    return SC_OK;
}
