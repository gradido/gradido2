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
 * ### Three threads, one registration
 *
 *   loop     bc_registration_prepare   the request's values into the unit, checked
 *   worker   registration_work         draw, write, and say how the transaction ends
 *   loop     done -> bc_registration_report, then the route's answer
 *
 * The draw happens on the worker, per attempt: a collision is answered with SC_DB_AGAIN, the
 * executor rolls the transaction back and runs the work again, and the next attempt draws fresh
 * values. The log line that says an account exists is written on the loop, afterwards, because
 * only then is it known that the COMMIT the work asked for went through.
 *
 * Nothing is cached: an account that did not exist a moment ago is in no session, and the member
 * cannot sign in until the address is confirmed. There is no invalidation to make visible here.
 */
#include "backend_core/domain/user.h"

#include <stdio.h>
#include <string.h>

#include "service_core/log/log.h"

/* On the worker, once per attempt. */
static sc_db_end registration_work(sc_db *db, sc_db_unit *unit)
{
    bc_registration *r = (bc_registration *)unit;
    bc_create_account_result result;
    sc_status status;

    /* Drawn, not checked. Both are answered by an index at the moment of the write, and a
     * collision comes back below as something to draw again. */
    bc_new_gradido_id(r->account.gradido_id);
    r->account.email_verification_code = bc_new_email_verification_code();
    r->account.created_at = sc_now_ms();

    status = bc_user_create_account(db, &r->account, &result, r->error, sizeof(r->error));
    if (status != SC_OK) {
        r->outcome = BC_REGISTRATION_FAILED;
        return SC_DB_ROLLBACK;
    }

    switch (result.outcome) {
    case BC_ACCOUNT_COLLIDED: {
        /* Two random values can land on one that exists, and they are not equally unlikely: a
         * v4 gradido_id is 122 bits and will not happen, while the verification code is bounded
         * to 2^53-1 for SQLite's sake, where a community of a million members has a birthday
         * chance of roughly one in eighteen thousand over its whole life.
         * At warn rather than passed over, because a run of these is not luck: it is a
         * generator that has stopped being random, and the only way anybody finds out is if the
         * rare case says something when it happens. */
        sc_log_value data[2] = {SC_LOG_STR("constraint", result.constraint),
                                SC_LOG_INT("attempt", unit->attempt)};
        sc_log_context log = {0};

        log.data = data;
        log.data_count = 2;
        sc_log_event(SC_LOG_WARN, SC_CAT_USER, "user.registration.collision", &log,
                     "a generated value was already taken, drawing again");
        if (unit->attempt < BC_REGISTER_MAX_ATTEMPTS)
            return SC_DB_AGAIN;
        /* Every draw landed on a value that exists. At these widths that is not a coincidence
         * happening five times, it is a generator that has stopped generating -- so this is a
         * failure and not another attempt, and the route answers 500 rather than the silent
         * 204 that would tell somebody their registration went through. */
        (void)snprintf(r->error, sizeof(r->error), "no free generated values after %d attempts",
                       BC_REGISTER_MAX_ATTEMPTS);
        r->outcome = BC_REGISTRATION_FAILED;
        return SC_DB_ROLLBACK;
    }
    case BC_ACCOUNT_ADDRESS_TAKEN:
        /* Rolled back: the member row written before the conflict was known goes with it. */
        r->outcome = BC_REGISTRATION_ADDRESS_TAKEN;
        r->user_id = result.taken_by;
        return SC_DB_ROLLBACK;
    case BC_ACCOUNT_CREATED:
    default:
        r->outcome = BC_REGISTRATION_CREATED;
        r->user_id = result.user_id;
        return SC_DB_COMMIT;
    }
}

sc_status bc_registration_prepare(bc_registration *registration, const bc_home_community *home,
                                  const char *first_name, const char *last_name, const char *email,
                                  const char *language, char *error, size_t error_size)
{
    bc_new_account *account;

    if (registration == NULL || home == NULL || first_name == NULL || last_name == NULL ||
        email == NULL || error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;
    error[0] = '\0';
    memset(registration, 0, sizeof(*registration));
    account = &registration->account;

    if (!bc_normalize_email(email, account->email, sizeof(account->email))) {
        bc_sql_set_error(error, error_size, "the email address does not fit its column");
        return SC_ERR_TOO_LONG;
    }
    if (strlen(first_name) + 1 > sizeof(account->first_name) ||
        strlen(last_name) + 1 > sizeof(account->last_name)) {
        bc_sql_set_error(error, error_size, "a name does not fit its column");
        return SC_ERR_TOO_LONG;
    }
    (void)snprintf(account->first_name, sizeof(account->first_name), "%s", first_name);
    (void)snprintf(account->last_name, sizeof(account->last_name), "%s", last_name);
    (void)snprintf(account->language, sizeof(account->language), "%s",
                   bc_language_or_default(language));
    /* The community this instance is. It is on the context rather than looked up here: one row,
     * written once at setup, and the process refuses to start without it. */
    account->community_id = home->id;

    registration->unit.access = SC_DB_WRITE;
    registration->unit.work = registration_work;
    return SC_OK;
}

void bc_registration_report(const bc_registration *registration)
{
    sc_log_value data[1];
    sc_log_context log = {0};

    /* Only what finished: a unit the executor did not run, or whose COMMIT was refused, left no
     * account and gets no line here -- its caller says what happened instead. */
    if (registration == NULL || registration->unit.status != SC_OK)
        return;
    log.usr = registration->user_id;
    log.data = data;
    log.data_count = 1;

    switch (registration->outcome) {
    case BC_REGISTRATION_CREATED:
        data[0] = (sc_log_value)SC_LOG_STR("language", registration->account.language);
        sc_log_event(SC_LOG_INFO, SC_CAT_USER, "user.registration.created", &log,
                     "account created");
        break;
    case BC_REGISTRATION_ADDRESS_TAKEN:
        /* Legacy mails the member who *owns* the address -- in their language and with their
         * name, never the new registrant's -- so that somebody typing the wrong address is
         * noticed by the person who would otherwise never hear about it. The reference path has
         * the same TODO and the same reason: no role sends mail yet, and loading a whole member
         * for it would be a round trip spent on a comment. */
        data[0] = (sc_log_value)SC_LOG_STR("reason", "address-in-use");
        sc_log_event(SC_LOG_INFO, SC_CAT_USER, "user.registration.denied", &log,
                     "registration for an address that is already in use, answering as if it "
                     "were new");
        break;
    case BC_REGISTRATION_PENDING:
    case BC_REGISTRATION_FAILED:
    default:
        break;
    }
}

sc_status bc_register_account_on(sc_db *db, const bc_home_community *home, const char *first_name,
                                 const char *last_name, const char *email, const char *language,
                                 char *error, size_t error_size)
{
    bc_registration registration;
    sc_status status;

    status = bc_registration_prepare(&registration, home, first_name, last_name, email, language,
                                     error, error_size);
    if (status != SC_OK)
        return status;
    status = sc_db_run(db, &registration.unit);
    if (status != SC_OK) {
        bc_sql_set_error(error, error_size, registration.unit.error.message);
        return status;
    }
    bc_registration_report(&registration);
    if (registration.outcome == BC_REGISTRATION_FAILED) {
        (void)snprintf(error, error_size, "%s", registration.error);
        return SC_ERR_UNAVAILABLE;
    }
    return SC_OK;
}
