/*
 * The community domain: what a community is here, and how the one this instance *is* is read
 * and written.
 *
 * contracts/db/communities.json is the authority on what a row holds; this is what the code
 * that reads one passes around. The counterpart on the TypeScript path is
 * packages/backend-core/src/domain/community/.
 */
#ifndef BACKEND_CORE_COMMUNITY_H
#define BACKEND_CORE_COMMUNITY_H

#include <stddef.h>
#include <stdint.h>

#include "backend_core/database/sql.h"
#include "backend_core/uuid.h"
#include "service_core/db.h"
#include "service_core/status.h"

/** contracts/db/communities.json -- `name varchar(40)`, and the terminator. */
#define BC_COMMUNITY_NAME_MAX 41
/** contracts/db/communities.json -- `description varchar(255)`. */
#define BC_COMMUNITY_DESCRIPTION_MAX 256
/** contracts/db/communities.json -- `url varchar(255)`. */
#define BC_COMMUNITY_URL_MAX 256
/** ed25519, and the layout libsodium and legacy's dht-node both use. */
#define BC_PUBLIC_KEY_SIZE 32
/** The secret key: the 32 byte seed followed by the 32 byte public key. */
#define BC_PRIVATE_KEY_SIZE 64

/**
 * This instance's own community, as the rest of the process refers to it.
 *
 * **The private key is deliberately not in it.** This value is held for the lifetime of the
 * process and read by anything that has a context -- which is the last place a secret should be.
 * Whatever comes to need it (signing a federation handshake, signing a transaction) loads it at
 * that moment, through a repository call written for that purpose, and lets go of it again.
 * contracts/logging.json forbids logging it, and the cheapest way to keep that promise is for
 * the object everyone carries not to contain it.
 *
 * The uuid *is* here: it is the community's public identity and the thing federation names it
 * by. That it lives on this object and in `communities.community_uuid` -- and in no other table
 * -- is the whole point of `users.community_id` being a row id.
 */
typedef struct bc_home_community {
    /** `communities.id`. What every other table references. */
    uint64_t id;
    char community_uuid[BC_UUID_TEXT_MAX];
    char url[BC_COMMUNITY_URL_MAX];
    char name[BC_COMMUNITY_NAME_MAX];
    /** The column is nullable; `has_description` is 0 for a row that holds NULL. An empty string
     *  is not a third state -- see the setup, which turns one into NULL. */
    char description[BC_COMMUNITY_DESCRIPTION_MAX];
    int has_description;
    uint8_t public_key[BC_PUBLIC_KEY_SIZE];
} bc_home_community;

/**
 * What a person has to say about their community, and nothing a machine can work out.
 *
 * These three are the whole of the first-run setup: the key pair, the uuid and the timestamps
 * are generated, and asking about them would only be a way to get them wrong. The bounds are the
 * columns -- a value that fits the form and not the column is a failure at the end of a setup
 * rather than during it.
 */
typedef struct bc_home_community_setup {
    char name[BC_COMMUNITY_NAME_MAX];
    char description[BC_COMMUNITY_DESCRIPTION_MAX];
    int has_description;
    char url[BC_COMMUNITY_URL_MAX];
} bc_home_community_setup;

/* --- logic -------------------------------------------------------------------------------- */

/**
 * A fresh ed25519 key pair for a community, in the two shapes the columns hold.
 *
 * contracts/db/communities.json says `public_key bytes(32)` and `private_key bytes(64)`, which
 * is libsodium's layout and what legacy's dht-node writes: the secret key is the 32 byte seed
 * followed by the 32 byte public key. Storing it that way is what lets a signature made on
 * either implementation be verified by anything that reads the row.
 *
 * This is *not* the SLIP-10 tree the shared package calls `CommunityKeyPair`. That one derives
 * member and account keys from a community root seed and belongs to the blockchain; this is the
 * community's own signing identity, which is what `communities.public_key` holds and what
 * federation identifies a community by. Two different keys with similar names -- do not merge
 * them.
 */
void bc_community_new_keys(uint8_t public_key[BC_PUBLIC_KEY_SIZE],
                           uint8_t private_key[BC_PRIVATE_KEY_SIZE]);

/* --- repository ---------------------------------------------------------------------------- */

/**
 * This instance's own community, or nothing on a database that has never been set up.
 *
 * `remote = false` on exactly one row, by contract. If there were ever two this would quietly
 * pick one, so it does not: a second home community is a broken database, not a situation to
 * cope with, and it is reported rather than tolerated.
 *
 * @p found is set to 0 for an empty database and 1 when @p out was filled. `private_key` is not
 * selected: it is a secret with exactly one future reader, and until that exists nothing loads
 * it.
 */
sc_status bc_community_find_home(sc_db *db, bc_home_community *out, int *found, char *error,
                                 size_t error_size);

/* --- interaction --------------------------------------------------------------------------- */

/**
 * The instance becomes a community.
 *
 * Runs once, at the first start against an empty database, and everything that follows depends
 * on it: `users.community_id` is NOT NULL, so no member can exist before this row does, and the
 * backend refuses to serve without it.
 *
 * The caller supplies only what a person knows. The uuid, the key pair and the timestamps are
 * made here. The private key is written and then left alone -- it does not go into the
 * bc_home_community this answers with, for the reason on that type.
 */
sc_status bc_create_home_community(sc_db *db, const bc_home_community_setup *setup,
                                   bc_home_community *out, char *error, size_t error_size);

#endif /* BACKEND_CORE_COMMUNITY_H */
