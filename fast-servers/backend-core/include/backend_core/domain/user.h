/*
 * The user domain: what an account is here, how one is looked up and written, and what happens
 * when somebody signs up.
 *
 * contracts/db/users.json and contracts/db/user_contacts.json are the authority on what a row
 * holds; this is what the code that creates one passes around. The counterpart on the TypeScript
 * path is packages/backend-core/src/domain/user/.
 */
#ifndef BACKEND_CORE_USER_H
#define BACKEND_CORE_USER_H

#include <stddef.h>
#include <stdint.h>

#include "backend_core/database/sql.h"
#include "backend_core/language.h"
#include "backend_core/uuid.h"
#include "service_core/db.h"
#include "service_core/status.h"

/** contracts/db/user_contacts.json -- `email varchar(255)`, and the terminator. */
#define BC_EMAIL_MAX 256
/** contracts/db/users.json -- `first_name` and `last_name` are `varchar(255)`. */
#define BC_NAME_MAX 256

/** Everything an account is written from, decided before the first row is touched. */
typedef struct bc_new_account {
    /** Trimmed and lowercased. The comparison in `user_contacts.email` is exact. */
    char email[BC_EMAIL_MAX];
    char first_name[BC_NAME_MAX];
    char last_name[BC_NAME_MAX];
    char language[BC_LANGUAGE_MAX];
    /** `users.community_id` -- a row id, never the community's uuid. */
    uint64_t community_id;
    /** `users.gradido_id`. Made by the interaction, not by the database, and free in that
     *  community: `users_uuid_key` is `(gradido_id, community_id)`. */
    char gradido_id[BC_UUID_TEXT_MAX];
    /** `user_contacts.email_verification_code`. A secret: never logged, and never in a response
     *  -- which `user.create` makes easy, since it has no response body at all. */
    uint64_t email_verification_code;
    /** One instant for both rows, so the account has a single moment of creation. */
    int64_t created_at;
} bc_new_account;

/**
 * Where the login address leads.
 *
 * Deliberately not the whole account: what asks for it is the check whether an address is
 * already in use, and that answer must not turn into a way to read a stranger's profile. The
 * name and language are here because the mail that goes out in that case is addressed to the
 * member who *owns* the address, in their language -- never in the new registrant's.
 */
typedef struct bc_address_owner {
    uint64_t id;
    char first_name[BC_NAME_MAX];
    char last_name[BC_NAME_MAX];
    char language[BC_LANGUAGE_MAX];
} bc_address_owner;

/* --- logic --------------------------------------------------------------------------------- */

/**
 * The one normalization every lookup and every write agrees on: trimmed and lowercased.
 *
 * ASCII, and that is exact rather than approximate here: the address has been through the
 * contracted email rule by the time this sees it, and that rule admits no byte above 0x7f.
 * Answers 0 for an address that would not fit @p out.
 */
int bc_normalize_email(const char *email, char *out, size_t out_size);

/**
 * `user_contacts.email_verification_code` -- 53 random bits, never zero.
 *
 * The width is not a security decision, it is the widest value that survives both databases
 * unchanged. Legacy draws 64 bits into a MariaDB `bigint unsigned`; PostgreSQL `bigint` and
 * SQLite `INTEGER` are both *signed*, so the top bit is gone before anything is stored, and
 * SQLite hands an INTEGER to JavaScript as a double, which quietly rounds anything past 2^53-1.
 * A code that comes back changed is a link that does not work and a row that cannot be found --
 * with nothing failing anywhere. contracts/db/user_contacts.json records the bound, and this
 * implementation keeps to it although C would not round: the column is shared.
 *
 * Zero is excluded because it is what an unset column looks like, and a code nobody was sent
 * must not match a row.
 */
uint64_t bc_new_email_verification_code(void);

/** How many draws bc_new_gradido_id gives up after. See its comment. */
#define BC_GRADIDO_ID_MAX_DRAWS 5

/**
 * A `users.gradido_id` nobody in this community holds yet.
 *
 * **Unique per community, not globally** -- contracts/db/users.json, `uuid_key`. A v4 uuid from
 * the system CSPRNG makes a collision unlikely, and unlikely is not impossible: the check is what
 * makes the rule true rather than probable, and it costs one indexed lookup on a path that is
 * already writing three rows.
 *
 * The asymmetry with `alias` is the whole reason this exists as code rather than as a constraint
 * alone: a *generated* value that collides is simply drawn again and nobody notices, while a
 * *chosen* alias that collides is a person being told no.
 *
 * @p exists is a parameter rather than a repository call, so the ladder can be tested without a
 * database and so this stays free of persistence. It answers 1 for taken, 0 for free and -1 when
 * it could not tell, which ends the draw rather than counting as free.
 *
 * Answers SC_ERR_UNAVAILABLE after BC_GRADIDO_ID_MAX_DRAWS. Reaching that is not a collision --
 * at 122 random bits, five in a row is not a number that happens. It is @p exists answering true
 * for reasons of its own, and a loop that would spin forever on it is worse than an error that
 * says so.
 */
sc_status bc_new_gradido_id(int (*exists)(const char *gradido_id, void *user_data), void *user_data,
                            char *out);

/* --- repository ------------------------------------------------------------------------------ */

/**
 * Who holds this address, if anybody. @p found is 0 when nobody does.
 *
 * Deleted members are excluded: a soft-deleted row still occupies the unique index on
 * `user_contacts.email`, so this answers "can this address be registered" correctly only for the
 * living. Reviving a deleted account is a different operation and does not exist yet -- until it
 * does, an address belonging to a deleted member is unusable, which is what legacy does too.
 */
sc_status bc_user_find_address_owner(sc_db *db, const char *email, bc_address_owner *out,
                                     int *found, char *error, size_t error_size);

/**
 * Whether this community already has a member with that gradido id.
 *
 * Scoped by community because `users_uuid_key` is, and asking table-wide would be a different
 * question: another community's member holding the same uuid is not a conflict, it is what the
 * two-column key exists to allow.
 *
 * It does not replace the index -- between this select and the insert there is a window, and only
 * the index closes it. It is here because a *generated* value that turns out to be taken can
 * simply be drawn again.
 */
sc_status bc_user_gradido_id_exists(sc_db *db, const char *gradido_id, uint64_t community_id,
                                    int *exists, char *error, size_t error_size);

/**
 * Writes the member and their login address, or neither, and answers with `users.id`.
 *
 * Just the id: everything else about a new account is what the caller passed in, and a repository
 * that echoed it back would only invite somebody to read it as confirmation.
 *
 * Three statements, because the two rows point at each other: the member exists before the
 * contact can name them, and `users.email_id` can only be written once the contact has an id.
 * Inside one transaction, so an account without an address -- which nothing could log into and
 * nothing would report -- cannot survive a failure halfway through.
 */
sc_status bc_user_create_account(sc_db *db, const bc_new_account *account, uint64_t *id_out,
                                 char *error, size_t error_size);

/* --- interaction ----------------------------------------------------------------------------- */

struct bc_context;

/**
 * Somebody signs up. See register_account.c for what it does and what it deliberately does not.
 *
 * The four values are the contracted request minus the fields no interaction reads yet, and they
 * arrive already checked and trimmed -- validating a body is what the route owns, and doing it
 * twice would be two places for the rule to live. @p language may be NULL or unknown; it becomes
 * the default, which is the contract's ignore_and_warn policy.
 *
 * Answers SC_OK for a registration that was written **and** for one that was answered as if it
 * had been: the silence rule is that the caller cannot tell, and that starts here rather than at
 * the route.
 *
 * Takes @p context's database lock for its whole length, which is why the context is not const
 * here -- see bc_context.db_lock for what that lock is and what it is standing in for.
 */
sc_status bc_register_account(struct bc_context *context, const char *first_name,
                              const char *last_name, const char *email, const char *language,
                              char *error, size_t error_size);

#endif /* BACKEND_CORE_USER_H */
