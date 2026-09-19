/*
 * "This node key belongs to this community until then", signed by the community key: the
 * delegation every frame of the peer network carries. The format is libp2p-ffi's, byte for byte;
 * contracts/test-vectors/master-seed.json holds delegations the module signed itself.
 * `packages/shared/src/crypto/dhtDelegation.ts` is the other half.
 *
 *   node key (32) | community key (32) | expires_ms, big endian (8, 0 = never) | signature (64)
 *   signature: ed25519 by the community key over
 *              "libp2p-ffi delegation v1" || node key || community key || expires_ms
 */
#ifndef SERVICE_CORE_DHT_DELEGATION_H
#define SERVICE_CORE_DHT_DELEGATION_H

#include <stdint.h>

#include "service_core/status.h"

#define SC_DHT_DELEGATION_BYTES 136
/** A delegation as DHT_DELEGATION holds it: hex, and the terminator. */
#define SC_DHT_DELEGATION_HEX_SIZE (2 * SC_DHT_DELEGATION_BYTES + 1)

/**
 * Signs a delegation for @p node_key. @p community_private_key is the column's 64 bytes: seed,
 * then public key. SC_ERR_UNAVAILABLE when libsodium does not initialise.
 */
sc_status sc_dht_delegation_sign(const uint8_t community_private_key[64],
                                 const uint8_t node_key[32], uint64_t expires_ms,
                                 uint8_t out[SC_DHT_DELEGATION_BYTES]);

#endif /* SERVICE_CORE_DHT_DELEGATION_H */
