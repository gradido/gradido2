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
#include "backend_core/domain/community.h"
#include "backend_core/language.h"
#include "backend_core/uuid.h"
#include "service_core/db.h"
#include "service_core/db_exec.h"
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

/**
 * A value for `users.gradido_id`.
 *
 * **Unique per community, not globally** -- contracts/db/users.json, `uuid_key`. This draws and
 * hands over; it does not ask whether the value is free. `users_uuid_key` answers that question
 * at the moment of the write, which is the only moment the answer is still true, and a lookup
 * before it would be a round trip spent on an answer that can go stale between the reading and
 * the writing.
 *
 * So a collision is not prevented here, it is *survived*: bc_register_account is told
 * BC_ACCOUNT_COLLIDED, says so in the log and draws again. At 122 random bits from the system
 * CSPRNG that path is not one anybody will see -- which is exactly why it has to be written down
 * rather than assumed away, because nothing that never runs is ever noticed to be wrong.
 *
 * The asymmetry with `alias` is the reason this exists as a function rather than as a constraint
 * alone: a *generated* value that collides is drawn again and nobody notices, while a *chosen*
 * alias that collides is a person being told no.
 */
void bc_new_gradido_id(char *out);

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

/** How a refused write is named back to the caller, where the driver names it. */
#define BC_CONSTRAINT_MAX 128

/** What became of the three writes. Every one of these is a normal end, not a failure. */
typedef enum bc_account_outcome {
    /** The rows are written and `user_id` holds `users.id`. */
    BC_ACCOUNT_CREATED = 0,
    /** `user_contacts_email_key` already holds the address; `taken_by` is whose it is. */
    BC_ACCOUNT_ADDRESS_TAKEN,
    /** A *generated* value landed on one that exists, and `constraint` names which where the
     *  driver says so. Never the address: that is BC_ACCOUNT_ADDRESS_TAKEN, which is an answer
     *  to give rather than a draw to repeat. */
    BC_ACCOUNT_COLLIDED
} bc_account_outcome;

/**
 * Which of the three happened, and the one value that goes with it.
 *
 * One struct rather than three out parameters, because the fields are mutually exclusive: a
 * caller that reads `user_id` after BC_ACCOUNT_ADDRESS_TAKEN is reading a zero, and putting them
 * behind an outcome it has to switch on is what makes that hard to do by accident.
 */
typedef struct bc_create_account_result {
    bc_account_outcome outcome;
    uint64_t user_id;
    uint64_t taken_by;
    /** libpq gives the constraint's own name, SQLite the columns; empty when neither said. */
    char constraint[BC_CONSTRAINT_MAX];
} bc_create_account_result;

/**
 * Writes the member and their login address, or neither, and says which of the three happened.
 *
 * **The unique index decides, not a lookup before it.** `user_contacts_email_key` is what makes
 * an address one member's, so this asks it by writing rather than by selecting first: a lookup
 * answers about a moment that is over by the time the insert runs, and two registrations for one
 * free address would both find it free -- one would then fail on the index, which is a 500 that
 * happens only for addresses that were *not* yet registered. That is the membership oracle
 * contracts/server/backend/user.json closes with one empty 204 for both cases, and no rule
 * outside the database can keep it closed.
 *
 * `ON CONFLICT (email) DO NOTHING` rather than letting the insert fail, because three unique
 * constraints can refuse this row and they mean different things: the address being taken is the
 * contracted silence, while a collision on `email_verification_code` or on `users_uuid_key` is a
 * coincidence to draw again for. No row returned says "the address" and nothing else, without
 * reading an error message -- which on SQLite would be parsing English.
 *
 * Three statements, because the two rows point at each other: the member exists before the
 * contact can name them, and `users.email_id` -- which of several addresses mail goes to -- can
 * only be written once the contact has an id.
 *
 * **Inside the caller's transaction, and it opens none.** The unit that calls this is a write
 * unit, run by the executor between a BEGIN and an end it chooses: COMMIT for
 * BC_ACCOUNT_CREATED, ROLLBACK for the address being taken -- which takes the member row
 * written before the conflict was known back out -- and AGAIN for a collision, which rolls back
 * and draws fresh values. So an account without an address cannot survive a failure halfway
 * through. service_core/db_exec.h.
 *
 * Answers SC_OK for all three outcomes. A non-OK status is the database having gone wrong.
 */
sc_status bc_user_create_account(sc_db *db, const bc_new_account *account,
                                 bc_create_account_result *out, char *error, size_t error_size);

/* --- interaction ----------------------------------------------------------------------------- */

/** How many times a registration draws before the draw itself is taken to be broken. */
#define BC_REGISTER_MAX_ATTEMPTS 5

/** How a registration ended, once it has. */
typedef enum bc_registration_outcome {
    /** Not run yet, or the executor did not run it -- see the unit's status. */
    BC_REGISTRATION_PENDING = 0,
    /** Two rows written. */
    BC_REGISTRATION_CREATED,
    /** The address is somebody's; nothing written, and the caller answers as if it were new. */
    BC_REGISTRATION_ADDRESS_TAKEN,
    /** The database refused, or every draw collided. `error` says which. */
    BC_REGISTRATION_FAILED
} bc_registration_outcome;

/**
 * Somebody signs up: the unit of database work that does it, and what it found.
 *
 * Built in the request's own memory by the route, prepared on the loop, run by the executor on
 * a worker, and read back on the loop in the unit's `done` -- see register_account.c for what it
 * does and what it deliberately does not, and service_core/db_exec.h for how a unit travels.
 */
typedef struct bc_registration {
    /** First, so the executor's unit and this are one address. */
    sc_db_unit unit;
    bc_new_account account;
    bc_registration_outcome outcome;
    /** `users.id` of the account written, or of the member who holds the address. */
    uint64_t user_id;
    char error[BC_SQL_ERROR_MAX];
} bc_registration;

/**
 * Fills @p registration from the request, on the loop: the address normalized, the names and
 * language checked against their columns, the community set. Nothing is drawn here -- the
 * generated values are drawn by the work, once per attempt.
 *
 * The four values are the contracted request minus the fields no interaction reads yet, and
 * they arrive already checked and trimmed -- validating a body is what the route owns. @p
 * language may be NULL or unknown; it becomes the default, which is the contract's
 * ignore_and_warn policy.
 *
 * Sets the unit's access and work; the caller sets `done` and submits it. Answers
 * SC_ERR_TOO_LONG for a value that does not fit its column, with @p error saying which.
 */
sc_status bc_registration_prepare(bc_registration *registration, const bc_home_community *home,
                                  const char *first_name, const char *last_name, const char *email,
                                  const char *language, char *error, size_t error_size);

/**
 * Writes the log line a finished registration is contracted to leave -- created, or denied for
 * an address in use -- and nothing for one that did not finish. On the loop, from `done`: only
 * there is it known that the transaction the work asked for actually committed.
 */
void bc_registration_report(const bc_registration *registration);

/**
 * The whole of it, right here on @p db: prepare, run, report. For the setup command and for
 * tests, which have a connection and no executor.
 *
 * Answers SC_OK for a registration that was written **and** for one that was answered as if it
 * had been: the silence rule is that the caller cannot tell, and that starts here rather than at
 * the route.
 */
sc_status bc_register_account_on(sc_db *db, const bc_home_community *home, const char *first_name,
                                 const char *last_name, const char *email, const char *language,
                                 char *error, size_t error_size);

#endif /* BACKEND_CORE_USER_H */
