/*
 * The root of every key this instance derives rather than stores -- MASTER_SEED in
 * contracts/secrets.json.
 *
 * A SLIP-10 tree through gradido-blockchain-core, the same C function the reference path calls
 * through shared-native, so the two cannot derive different identities from one seed.
 * contracts/test-vectors/master-seed.json holds both to that; `packages/shared/src/crypto/
 * masterSeed.ts` is the other half.
 */
#ifndef SERVICE_CORE_MASTER_SEED_H
#define SERVICE_CORE_MASTER_SEED_H

#include <stdint.h>

#include "service_core/status.h"

/** MASTER_SEED_BYTES in contracts/const.json. */
#define SC_MASTER_SEED_BYTES 32
/** The seed as MASTER_SEED holds it: hex, and the terminator. */
#define SC_MASTER_SEED_HEX_SIZE (2 * SC_MASTER_SEED_BYTES + 1)
/** MASTER_SEED_PATH_DHT in contracts/const.json: the ASCII bytes of `dht`. */
#define SC_MASTER_SEED_PATH_DHT 0x00646874u

/** @p text as a seed into @p out: 1 for exactly 64 hex digits, either case, and 0 otherwise. */
int sc_master_seed_parse(const char *text, uint8_t out[SC_MASTER_SEED_BYTES]);

/** @p seed as the 64 lowercase hex digits setup writes. */
void sc_master_seed_to_hex(const uint8_t seed[SC_MASTER_SEED_BYTES],
                           char out[SC_MASTER_SEED_HEX_SIZE]);

/**
 * A new seed: BLAKE2b-256 over 32 bytes of the operating system's randomness, the wall clock, the
 * monotonic clock and @p typed, which may be NULL.
 *
 * The operating system's randomness is what makes it secret; the rest only stands in for it should
 * that source ever be broken. SC_ERR_UNAVAILABLE when libsodium does not initialise.
 */
sc_status sc_master_seed_new(const char *typed, uint8_t out[SC_MASTER_SEED_BYTES]);

/**
 * The dht-node identity: one hardened step along `dht` from the seed's root. @p node_seed is what
 * libp2p-ffi takes as its node seed, @p node_key the public key a delegation names.
 */
sc_status sc_master_seed_derive_dht(const uint8_t seed[SC_MASTER_SEED_BYTES], uint8_t node_seed[32],
                                    uint8_t node_key[32]);

#endif /* SERVICE_CORE_MASTER_SEED_H */
