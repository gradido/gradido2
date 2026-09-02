/*
 * Somebody signs up.
 *
 * The behavioral reference is packages/backend-core's `registerAccount`, which is itself the
 * first slice of legacy's `createUser` resolver: **two rows and nothing else**. What that file
 * lists as still to come is not repeated here -- it is one list, it lives there, and a copy of it
 * in C would be a second thing to keep in step.
 *
 * Two properties of it are not deferred, because they are the ones that stop being addable later:
 *
 * **The silence rule.** A registration for an address that already exists answers exactly like
 * one for an address that does not -- an empty 204, no delay worth measuring, and no row written.
 * `user_contacts.email` is globally unique, so the alternative is not a neutral answer but a
 * constraint violation, and a 500 that only ever happens for registered addresses is a membership
 * oracle for anyone with a list of email addresses.
 *
 * **One instant, one transaction.** Both rows carry the same `created_at` and are written
 * together or not at all -- see bc_user_create_account.
 *
 * Nothing is cached: an account that did not exist a moment ago is in no session, and the member
 * cannot sign in until the address is confirmed. There is no invalidation to make visible here.
 */
#include "backend_core/backend_core.h"
#include "backend_core/domain/user.h"

#include <stdio.h>
#include <string.h>

#include "service_core/log.h"

/** What the gradido id ladder asks the database, carried through as its user_data. */
typedef struct gradido_id_probe {
    sc_db *db;
    uint64_t community_id;
    char *error;
    size_t error_size;
} gradido_id_probe;

static int gradido_id_taken(const char *gradido_id, void *user_data)
{
    gradido_id_probe *probe = (gradido_id_probe *)user_data;
    int exists = 0;

    if (bc_user_gradido_id_exists(probe->db, gradido_id, probe->community_id, &exists, probe->error,
                                  probe->error_size) != SC_OK)
        return -1;
    return exists;
}

/* The interaction, with the database already taken. The lock and the work are separated so that
 * every `return` below is one return and not one release plus one return. */
static sc_status register_account_locked(bc_context *context, const char *first_name,
                                         const char *last_name, const char *email,
                                         const char *language, char *error, size_t error_size)
{
    bc_address_owner owner;
    bc_new_account account;
    gradido_id_probe probe;
    uint64_t user_id = 0;
    int found = 0;
    sc_status status;

    if (context == NULL || first_name == NULL || last_name == NULL || email == NULL ||
        error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;
    error[0] = '\0';
    memset(&account, 0, sizeof(account));

    if (!bc_normalize_email(email, account.email, sizeof(account.email))) {
        bc_sql_set_error(error, error_size, "the email address does not fit its column");
        return SC_ERR_TOO_LONG;
    }

    status =
        bc_user_find_address_owner(context->db, account.email, &owner, &found, error, error_size);
    if (status != SC_OK)
        return status;
    if (found) {
        sc_log_value data[1] = {SC_LOG_STR("reason", "address-in-use")};
        sc_log_context log = {0};

        log.usr = owner.id;
        log.data = data;
        log.data_count = 1;
        sc_log_event(SC_LOG_INFO, SC_CAT_USER, "user.registration.denied", &log,
                     "registration for an address that is already in use, answering as if it "
                     "were new");

        /* Legacy mails the member who *owns* the address -- in their language and with their
         * name, never the new registrant's -- so that somebody typing the wrong address is
         * noticed by the person who would otherwise never hear about it. The reference path has
         * the same TODO and the same reason: no role sends mail yet. */
        return SC_OK;
    }

    if (strlen(first_name) + 1 > sizeof(account.first_name) ||
        strlen(last_name) + 1 > sizeof(account.last_name)) {
        bc_sql_set_error(error, error_size, "a name does not fit its column");
        return SC_ERR_TOO_LONG;
    }
    (void)snprintf(account.first_name, sizeof(account.first_name), "%s", first_name);
    (void)snprintf(account.last_name, sizeof(account.last_name), "%s", last_name);
    (void)snprintf(account.language, sizeof(account.language), "%s",
                   bc_language_or_default(language));
    /* The community this instance is. It is on the context rather than looked up here: one row,
     * written once at setup, and the process refuses to start without it. */
    account.community_id = context->home.id;
    account.email_verification_code = bc_new_email_verification_code();
    account.created_at = sc_now_ms();

    probe.db = context->db;
    probe.community_id = context->home.id;
    probe.error = error;
    probe.error_size = error_size;
    status = bc_new_gradido_id(gradido_id_taken, &probe, account.gradido_id);
    if (status != SC_OK) {
        if (status == SC_ERR_UNAVAILABLE)
            (void)snprintf(error, error_size, "no free gradido_id after %d draws",
                           BC_GRADIDO_ID_MAX_DRAWS);
        return status;
    }

    status = bc_user_create_account(context->db, &account, &user_id, error, error_size);
    if (status != SC_OK)
        return status;

    {
        sc_log_value data[1] = {SC_LOG_STR("language", account.language)};
        sc_log_context log = {0};

        log.usr = user_id;
        log.data = data;
        log.data_count = 1;
        sc_log_event(SC_LOG_INFO, SC_CAT_USER, "user.registration.created", &log,
                     "account created");
    }
    return SC_OK;
}

sc_status bc_register_account(bc_context *context, const char *first_name, const char *last_name,
                              const char *email, const char *language, char *error,
                              size_t error_size)
{
    sc_status status;

    if (context == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    /* The address is looked up and then written, and the two have to be one act: two of these
     * interleaving would both find the address free, and the second write would fail on the
     * unique index -- a 500 that only ever happens for addresses that are registered, which is
     * exactly the oracle the silence rule exists to close. See bc_context.db_lock. */
    bc_context_lock(context);
    status =
        register_account_locked(context, first_name, last_name, email, language, error, error_size);
    bc_context_unlock(context);
    return status;
}
