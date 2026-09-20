/*
 * The community vouches for this instance's dht node. The reference is
 * packages/backend-core/src/domain/community/interactions/sign-dht-delegation.ts.
 */
#include "backend_core/domain/community.h"

#include <sodium.h>
#include <stdio.h>

#include "service_core/dht_delegation.h"
#include "service_core/master_seed.h"

sc_status bc_sign_dht_delegation_for(sc_db *db, const uint8_t master_seed[32], uint8_t out[136],
                                     char *error, size_t error_size)
{
    uint8_t signing_key[BC_PRIVATE_KEY_SIZE];
    uint8_t node_seed[32];
    uint8_t node_key[32];
    int found = 0;
    sc_status status;

    _Static_assert(SC_DHT_DELEGATION_BYTES == 136, "delegation size");
    if (db == NULL || master_seed == NULL || out == NULL || error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;

    status = bc_community_find_home_signing_key(db, signing_key, &found, error, error_size);
    if (status != SC_OK)
        return status;
    if (!found) {
        (void)snprintf(error, error_size, "there is no home community to sign a delegation with");
        return SC_ERR_UNAVAILABLE;
    }

    status = sc_master_seed_derive_dht(master_seed, node_seed, node_key);
    if (status == SC_OK)
        status = sc_dht_delegation_sign(signing_key, node_key, 0, out);
    if (status != SC_OK)
        (void)snprintf(error, error_size, "the delegation could not be signed: %s",
                       sc_status_name(status));

    sodium_memzero(signing_key, sizeof(signing_key));
    sodium_memzero(node_seed, sizeof(node_seed));
    return status;
}
