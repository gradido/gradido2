/*
 * How an account is loaded and persisted. The interaction decides *when*.
 *
 * Every statement is written twice, once per dialect, side by side in one sc_sql_statement --
 * service_core/sql.h. The pair is the review unit: what differs between them is placeholder
 * syntax and how a boolean is spelled, and anything more than that should be visible at a
 * glance.
 *
 * Two things this file decides once, for every caller:
 *
 *   ids           an int64 from either database, widened to uint64_t here, once, so the domain
 *                 never has to know which database an id came from.
 *   transactions  none here. A repository runs statements; the unit that calls it runs inside
 *                 a transaction the executor opened and will end the way the unit says --
 *                 service_core/db_exec.h, *Transactions belong here*.
 */
#include "backend_core/domain/user.h"

#include <stdio.h>
#include <string.h>

#include "service_core/sql.h"

/* The join is the same question on both databases; only the placeholder differs. `deleted_at IS
 * NULL` is the half that makes this "can this address be registered" rather than "does this row
 * exist". */
static sc_sql_statement kFindOwner = SC_SQL_STATEMENT(
    "user.find_address_owner",
    "SELECT u.id, u.first_name, u.last_name, u.language FROM user_contacts c "
    "INNER JOIN users u ON u.id = c.user_id WHERE c.email = $1 AND u.deleted_at IS NULL LIMIT 1",
    "SELECT u.id, u.first_name, u.last_name, u.language FROM user_contacts c "
    "INNER JOIN users u ON u.id = c.user_id WHERE c.email = ?1 AND u.deleted_at IS NULL LIMIT 1");

/*
 * The three writes of an account, and the one read the address-taken branch needs.
 *
 *   'EMAIL'   contracts/types/UserContactType.json
 *   1         EMAIL_OPT_IN_REGISTER, contracts/types/OptInType.json
 *
 * Both are spelled into the statements rather than defaulted by the schema, because the
 * schema's defaults are for a row nobody described and these two are described. RETURNING on
 * both databases: DO NOTHING writes no row and leaves no id, so whether one came back is the
 * only thing that tells "written" from "the address is somebody's".
 */
static sc_sql_statement kInsertUser = SC_SQL_STATEMENT(
    "user.insert",
    "INSERT INTO users (gradido_id, community_id, first_name, last_name, language, created_at) "
    "VALUES ($1, $2, $3, $4, $5, $6) RETURNING id",
    "INSERT INTO users (gradido_id, community_id, first_name, last_name, language, created_at) "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6) RETURNING id");
static sc_sql_statement kInsertContact = SC_SQL_STATEMENT(
    "user_contact.insert",
    "INSERT INTO user_contacts (user_id, type, email, email_checked, email_verification_code, "
    "email_opt_in_type_id, created_at) VALUES ($1, 'EMAIL', $2, false, $3, 1, $4) "
    "ON CONFLICT (email) DO NOTHING RETURNING id",
    "INSERT INTO user_contacts (user_id, type, email, email_checked, email_verification_code, "
    "email_opt_in_type_id, created_at) VALUES (?1, 'EMAIL', ?2, 0, ?3, 1, ?4) "
    "ON CONFLICT (email) DO NOTHING RETURNING id");
static sc_sql_statement kOwnerOfAddress =
    SC_SQL_STATEMENT("user_contact.owner_of", "SELECT user_id FROM user_contacts WHERE email = $1",
                     "SELECT user_id FROM user_contacts WHERE email = ?1");
static sc_sql_statement kSetMainAddress =
    SC_SQL_STATEMENT("user.set_email_id", "UPDATE users SET email_id = $1 WHERE id = $2",
                     "UPDATE users SET email_id = ?1 WHERE id = ?2");

sc_status bc_user_find_address_owner(sc_db *db, const char *email, bc_address_owner *out,
                                     int *found, char *error, size_t error_size)
{
    sc_sql_param params[1];
    sc_sql_rows rows;
    sc_sql_error failure;
    sc_status status;

    if (db == NULL || email == NULL || out == NULL || found == NULL || error == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    *found = 0;
    error[0] = '\0';
    memset(out, 0, sizeof(*out));

    params[0] = sc_sql_text(email);
    status = sc_sql_query(db, &kFindOwner, params, 1, &rows, &failure);
    if (status != SC_OK) {
        bc_sql_set_error(error, error_size, failure.message);
        return status;
    }
    if (sc_sql_next(&rows)) {
        out->id = (uint64_t)sc_sql_col_int(&rows, 0);
        /* Both name columns are nullable in the contract, and legacy has rows that use it: NULL
         * reads as "", which is what the struct's empty string already means. */
        if (!sc_sql_col_copy(&rows, 1, out->first_name, sizeof(out->first_name)) ||
            !sc_sql_col_copy(&rows, 2, out->last_name, sizeof(out->last_name)) ||
            !sc_sql_col_copy(&rows, 3, out->language, sizeof(out->language))) {
            bc_sql_set_error(error, error_size, "a users row does not fit the contracted columns");
            status = SC_ERR_MALFORMED;
        } else {
            *found = 1;
        }
    }
    return bc_sql_finish(&rows, &failure, status, error, error_size);
}

/* What a refused write was: a generated value drawn before -- which the caller draws again for
 * -- or anything else, which is a failure. The address never lands here; DO NOTHING answers it. */
static sc_status refused(const sc_sql_error *failure, bc_create_account_result *out, char *error,
                         size_t error_size, sc_status status)
{
    if (failure->kind == SC_SQL_ERROR_UNIQUE) {
        out->outcome = BC_ACCOUNT_COLLIDED;
        (void)snprintf(out->constraint, sizeof(out->constraint), "%s", failure->constraint);
        return SC_OK;
    }
    bc_sql_set_error(error, error_size, failure->message);
    return status;
}

sc_status bc_user_create_account(sc_db *db, const bc_new_account *account,
                                 bc_create_account_result *out, char *error, size_t error_size)
{
    sc_sql_param user[6];
    sc_sql_param contact[4];
    sc_sql_param main_address[2];
    sc_sql_rows rows;
    sc_sql_error failure;
    int64_t user_id;
    int64_t contact_id;
    sc_status status;

    if (db == NULL || account == NULL || out == NULL || error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;
    error[0] = '\0';
    memset(out, 0, sizeof(*out));

    user[0] = sc_sql_text(account->gradido_id);
    user[1] = sc_sql_int((int64_t)account->community_id);
    user[2] = sc_sql_text(account->first_name);
    user[3] = sc_sql_text(account->last_name);
    user[4] = sc_sql_text(account->language);
    user[5] = sc_sql_time(account->created_at);
    status = sc_sql_query(db, &kInsertUser, user, 6, &rows, &failure);
    if (status != SC_OK)
        return refused(&failure, out, error, error_size, status);
    user_id = sc_sql_next(&rows) ? sc_sql_col_int(&rows, 0) : 0;
    status = bc_sql_finish(&rows, &failure, SC_OK, error, error_size);
    if (status != SC_OK)
        return status;

    /* The verification code is bounded to 2^53-1 by contract, so it is an int64 on both sides
     * without a bit to spare or to lose -- see bc_new_email_verification_code. */
    contact[0] = sc_sql_int(user_id);
    contact[1] = sc_sql_text(account->email);
    contact[2] = sc_sql_int((int64_t)account->email_verification_code);
    contact[3] = sc_sql_time(account->created_at);
    status = sc_sql_query(db, &kInsertContact, contact, 4, &rows, &failure);
    if (status != SC_OK)
        return refused(&failure, out, error, error_size, status);
    if (!sc_sql_next(&rows)) {
        /* No row, and -- asked before concluding anything -- no failure either: DO NOTHING did
         * nothing, which is what the address being taken looks like. A row that failed to come
         * back is not an address somebody holds. Whose it is, for the contracted `usr` on
         * user.registration.denied, is read in the same transaction, which the caller ends with a
         * rollback. */
        sc_sql_param address[1] = {sc_sql_text(account->email)};

        status = bc_sql_finish(&rows, &failure, SC_OK, error, error_size);
        if (status != SC_OK)
            return status;
        out->outcome = BC_ACCOUNT_ADDRESS_TAKEN;
        status = sc_sql_query(db, &kOwnerOfAddress, address, 1, &rows, &failure);
        if (status != SC_OK) {
            bc_sql_set_error(error, error_size, failure.message);
            return status;
        }
        if (sc_sql_next(&rows))
            out->taken_by = (uint64_t)sc_sql_col_int(&rows, 0);
        return bc_sql_finish(&rows, &failure, SC_OK, error, error_size);
    }
    contact_id = sc_sql_col_int(&rows, 0);
    status = bc_sql_finish(&rows, &failure, SC_OK, error, error_size);
    if (status != SC_OK)
        return status;

    main_address[0] = sc_sql_int(contact_id);
    main_address[1] = sc_sql_int(user_id);
    status = sc_sql_exec(db, &kSetMainAddress, main_address, 2, NULL, &failure);
    if (status != SC_OK)
        return refused(&failure, out, error, error_size, status);

    out->outcome = BC_ACCOUNT_CREATED;
    out->user_id = (uint64_t)user_id;
    return SC_OK;
}
