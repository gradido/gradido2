/*
 * How an account is loaded and persisted. The interaction decides *when*.
 *
 * Every statement is written twice, once per dialect, for the reason
 * community_repository.c states: Architecture.md, *Databases*, has no query surface both drivers
 * implement precisely so that a repository has to say which database it is talking to.
 *
 * Two things the branches do not share, and both are why an abstraction over them would be a lie:
 *
 *   ids           PostgreSQL hands back text this parses; SQLite hands back an integer. Widened
 *                 to uint64_t here, once, so the domain never has to know which database an id
 *                 came from.
 *   transactions  the three writes of an account are one transaction, and the statement that
 *                 opens it is the only part of that both dialects spell the same way.
 */
#include "backend_core/domain/user.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SC_DB_WITH_SQLITE)
#include <sqlite3.h>
#endif
#if defined(SC_DB_WITH_POSTGRESQL)
#include <libpq-fe.h>
#endif

/* The join is the same question on both databases; only the placeholder differs. `deleted_at IS
 * NULL` is the half that makes this "can this address be registered" rather than "does this row
 * exist". */
static const char kFindOwnerSqlite[] =
    "SELECT u.id, u.first_name, u.last_name, u.language FROM user_contacts c "
    "INNER JOIN users u ON u.id = c.user_id "
    "WHERE c.email = ? AND u.deleted_at IS NULL LIMIT 1";
static const char kFindOwnerPostgresql[] =
    "SELECT u.id, u.first_name, u.last_name, u.language FROM user_contacts c "
    "INNER JOIN users u ON u.id = c.user_id "
    "WHERE c.email = $1 AND u.deleted_at IS NULL LIMIT 1";

/** Both name columns are nullable in the contract; legacy has rows that use it. */
static int copy_text(char *out, size_t out_size, const char *text)
{
    if (text == NULL) {
        out[0] = '\0';
        return 1;
    }
    if (strlen(text) + 1 > out_size)
        return 0;
    (void)snprintf(out, out_size, "%s", text);
    return 1;
}

sc_status bc_user_find_address_owner(sc_db *db, const char *email, bc_address_owner *out,
                                     int *found, char *error, size_t error_size)
{
    if (db == NULL || email == NULL || out == NULL || found == NULL || error == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    *found = 0;
    error[0] = '\0';
    memset(out, 0, sizeof(*out));

    switch (sc_db_kind_of(db)) {
    case SC_DB_SQLITE: {
#if defined(SC_DB_WITH_SQLITE)
        sqlite3 *handle = (sqlite3 *)sc_db_native(db);
        sqlite3_stmt *statement = NULL;
        sc_status status = SC_OK;
        int step;

        if (sqlite3_prepare_v2(handle, kFindOwnerSqlite, -1, &statement, NULL) != SQLITE_OK) {
            bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
            return SC_ERR_INVALID_ARGUMENT;
        }
        sqlite3_bind_text(statement, 1, email, -1, SQLITE_STATIC);
        step = sqlite3_step(statement);
        if (step == SQLITE_ROW) {
            out->id = (uint64_t)sqlite3_column_int64(statement, 0);
            if (!copy_text(out->first_name, sizeof(out->first_name),
                           (const char *)sqlite3_column_text(statement, 1)) ||
                !copy_text(out->last_name, sizeof(out->last_name),
                           (const char *)sqlite3_column_text(statement, 2)) ||
                !copy_text(out->language, sizeof(out->language),
                           (const char *)sqlite3_column_text(statement, 3))) {
                bc_sql_set_error(error, error_size,
                                 "a users row does not fit the contracted columns");
                status = SC_ERR_MALFORMED;
            } else {
                *found = 1;
            }
        } else if (step != SQLITE_DONE) {
            bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
            status = SC_ERR_INVALID_ARGUMENT;
        }
        sqlite3_finalize(statement);
        return status;
#else
        bc_sql_set_error(error, error_size, "this build has no SQLite driver");
        return SC_ERR_UNAVAILABLE;
#endif
    }
    case SC_DB_POSTGRESQL:
    default: {
#if defined(SC_DB_WITH_POSTGRESQL)
        PGconn *handle = (PGconn *)sc_db_native(db);
        const char *params[1];
        PGresult *result;
        sc_status status = SC_OK;

        params[0] = email;
        result = PQexecParams(handle, kFindOwnerPostgresql, 1, NULL, params, NULL, NULL, 0);
        if (result == NULL || PQresultStatus(result) != PGRES_TUPLES_OK) {
            bc_sql_set_error(error, error_size,
                             result != NULL ? PQresultErrorMessage(result)
                                            : PQerrorMessage(handle));
            PQclear(result);
            return SC_ERR_INVALID_ARGUMENT;
        }
        if (PQntuples(result) == 1) {
            out->id = strtoull(PQgetvalue(result, 0, 0), NULL, 10);
            if (!copy_text(out->first_name, sizeof(out->first_name),
                           PQgetisnull(result, 0, 1) ? "" : PQgetvalue(result, 0, 1)) ||
                !copy_text(out->last_name, sizeof(out->last_name),
                           PQgetisnull(result, 0, 2) ? "" : PQgetvalue(result, 0, 2)) ||
                !copy_text(out->language, sizeof(out->language), PQgetvalue(result, 0, 3))) {
                bc_sql_set_error(error, error_size,
                                 "a users row does not fit the contracted columns");
                status = SC_ERR_MALFORMED;
            } else {
                *found = 1;
            }
        }
        PQclear(result);
        return status;
#else
        bc_sql_set_error(error, error_size, "this build has no PostgreSQL driver");
        return SC_ERR_UNAVAILABLE;
#endif
    }
    }
}

/*
 * The two rows, in the order they can be written.
 *
 *   'EMAIL'   contracts/types/UserContactType.json
 *   1         EMAIL_OPT_IN_REGISTER, contracts/types/OptInType.json
 *
 * Both are spelled here rather than defaulted by the schema, because the schema's defaults are
 * for a row nobody described and these two are described.
 */
#define BC_CONTACT_TYPE_EMAIL "EMAIL"
#define BC_OPT_IN_REGISTER 1

#if defined(SC_DB_WITH_SQLITE)
/**
 * Whether the last step was refused by a unique index rather than by anything else.
 *
 * A foreign key, a not-null or a full disk are not coincidences to draw again for, and answering
 * yes for them would turn a broken database into five silent retries and one 500 that says
 * nothing about what went wrong.
 */
static int sqlite_is_unique_violation(sqlite3 *handle)
{
    const int code = sqlite3_extended_errcode(handle);

    return code == SQLITE_CONSTRAINT_UNIQUE || code == SQLITE_CONSTRAINT_PRIMARYKEY;
}

/** The columns SQLite names, without its prefix. It has no constraint names to give. */
static void sqlite_constraint(sqlite3 *handle, char *out, size_t out_size)
{
    static const char kPrefix[] = "UNIQUE constraint failed: ";
    const char *said = sqlite3_errmsg(handle);
    const char *at = said != NULL ? strstr(said, kPrefix) : NULL;

    (void)snprintf(out, out_size, "%s", at != NULL ? at + sizeof(kPrefix) - 1 : "");
}

/** Whose the address is, read inside the transaction that just declined to overwrite it. */
static sc_status address_owner_sqlite(sqlite3 *handle, const char *email, uint64_t *out,
                                      char *error, size_t error_size)
{
    sqlite3_stmt *statement = NULL;
    sc_status status = SC_OK;
    int step;

    if (sqlite3_prepare_v2(handle, "SELECT user_id FROM user_contacts WHERE email = ? LIMIT 1", -1,
                           &statement, NULL) != SQLITE_OK) {
        bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
        return SC_ERR_INVALID_ARGUMENT;
    }
    sqlite3_bind_text(statement, 1, email, -1, SQLITE_STATIC);
    step = sqlite3_step(statement);
    if (step == SQLITE_ROW) {
        *out = (uint64_t)sqlite3_column_int64(statement, 0);
    } else if (step != SQLITE_DONE) {
        /* No row is not a possibility worth a branch -- the index just said the address is
         * there -- but a select that *failed* is, and answering 0 for it would put a zero in the
         * `usr` of a log line that is supposed to name somebody. */
        bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
        status = SC_ERR_INVALID_ARGUMENT;
    }
    sqlite3_finalize(statement);
    return status;
}

static sc_status create_account_sqlite(sqlite3 *handle, const bc_new_account *account,
                                       bc_create_account_result *out, char *error,
                                       size_t error_size)
{
    sqlite3_stmt *statement = NULL;
    sqlite3_int64 user_id;
    sqlite3_int64 contact_id;
    int step;

    if (sqlite3_prepare_v2(handle,
                           "INSERT INTO users (gradido_id, community_id, first_name, last_name, "
                           "language, created_at) VALUES (?, ?, ?, ?, ?, ?)",
                           -1, &statement, NULL) != SQLITE_OK)
        goto refused;
    sqlite3_bind_text(statement, 1, account->gradido_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)account->community_id);
    sqlite3_bind_text(statement, 3, account->first_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 4, account->last_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 5, account->language, -1, SQLITE_STATIC);
    sqlite3_bind_int64(statement, 6, account->created_at);
    if (sqlite3_step(statement) != SQLITE_DONE)
        goto refused;
    sqlite3_finalize(statement);
    statement = NULL;
    user_id = sqlite3_last_insert_rowid(handle);

    /* RETURNING rather than last_insert_rowid(): DO NOTHING writes no row and leaves the previous
     * insert's rowid in place, so whether a row came back is the only thing that distinguishes an
     * address that was written from one that somebody already holds. */
    if (sqlite3_prepare_v2(handle,
                           "INSERT INTO user_contacts (user_id, type, email, email_checked, "
                           "email_verification_code, email_opt_in_type_id, created_at) "
                           "VALUES (?, ?, ?, 0, ?, ?, ?) "
                           "ON CONFLICT (email) DO NOTHING RETURNING id",
                           -1, &statement, NULL) != SQLITE_OK)
        goto refused;
    sqlite3_bind_int64(statement, 1, user_id);
    sqlite3_bind_text(statement, 2, BC_CONTACT_TYPE_EMAIL, -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 3, account->email, -1, SQLITE_STATIC);
    sqlite3_bind_int64(statement, 4, (sqlite3_int64)account->email_verification_code);
    sqlite3_bind_int(statement, 5, BC_OPT_IN_REGISTER);
    sqlite3_bind_int64(statement, 6, account->created_at);
    step = sqlite3_step(statement);
    if (step == SQLITE_DONE) {
        /* Not a failure: DO NOTHING did nothing, which is what the address being taken looks
         * like. The member row written a moment ago goes away with the rollback above this. */
        sqlite3_finalize(statement);
        out->outcome = BC_ACCOUNT_ADDRESS_TAKEN;
        return address_owner_sqlite(handle, account->email, &out->taken_by, error, error_size);
    }
    if (step != SQLITE_ROW)
        goto refused;
    contact_id = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    statement = NULL;

    if (sqlite3_prepare_v2(handle, "UPDATE users SET email_id = ? WHERE id = ?", -1, &statement,
                           NULL) != SQLITE_OK)
        goto refused;
    sqlite3_bind_int64(statement, 1, contact_id);
    sqlite3_bind_int64(statement, 2, user_id);
    if (sqlite3_step(statement) != SQLITE_DONE)
        goto refused;
    sqlite3_finalize(statement);

    out->outcome = BC_ACCOUNT_CREATED;
    out->user_id = (uint64_t)user_id;
    return SC_OK;

refused:
    /* The address is answered above and never arrives here, so a unique violation at this point
     * is one of the two generated values having been drawn before. */
    if (sqlite_is_unique_violation(handle)) {
        out->outcome = BC_ACCOUNT_COLLIDED;
        sqlite_constraint(handle, out->constraint, sizeof(out->constraint));
        sqlite3_finalize(statement);
        return SC_OK;
    }
    bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
    sqlite3_finalize(statement);
    return SC_ERR_INVALID_ARGUMENT;
}
#endif

#if defined(SC_DB_WITH_POSTGRESQL)
/** SQLSTATE 23505, unique_violation -- 23503 is a foreign key and 23502 a not-null. */
static int pg_is_unique_violation(const PGresult *result)
{
    const char *state = result != NULL ? PQresultErrorField(result, PG_DIAG_SQLSTATE) : NULL;

    return state != NULL && strcmp(state, "23505") == 0;
}

/**
 * What a refused statement was: a collision to draw again for, or something to report.
 *
 * Clears @p result either way, so every caller is one line and none of them owns a PGresult past
 * the `if` that found it wrong.
 */
static sc_status pg_refused(PGresult *result, PGconn *handle, bc_create_account_result *out,
                            char *error, size_t error_size)
{
    if (pg_is_unique_violation(result)) {
        const char *name = PQresultErrorField(result, PG_DIAG_CONSTRAINT_NAME);

        out->outcome = BC_ACCOUNT_COLLIDED;
        (void)snprintf(out->constraint, sizeof(out->constraint), "%s", name != NULL ? name : "");
        PQclear(result);
        return SC_OK;
    }
    bc_sql_set_error(error, error_size,
                     result != NULL ? PQresultErrorMessage(result) : PQerrorMessage(handle));
    PQclear(result);
    return SC_ERR_INVALID_ARGUMENT;
}

/** Whose the address is, read inside the transaction that just declined to overwrite it. */
static sc_status address_owner_postgresql(PGconn *handle, const char *email, uint64_t *out,
                                          char *error, size_t error_size)
{
    const char *params[1];
    PGresult *result;

    params[0] = email;
    result = PQexecParams(handle, "SELECT user_id FROM user_contacts WHERE email = $1 LIMIT 1", 1,
                          NULL, params, NULL, NULL, 0);
    if (result == NULL || PQresultStatus(result) != PGRES_TUPLES_OK) {
        bc_sql_set_error(error, error_size,
                         result != NULL ? PQresultErrorMessage(result) : PQerrorMessage(handle));
        PQclear(result);
        return SC_ERR_INVALID_ARGUMENT;
    }
    if (PQntuples(result) == 1 && !PQgetisnull(result, 0, 0))
        *out = strtoull(PQgetvalue(result, 0, 0), NULL, 10);
    PQclear(result);
    return SC_OK;
}

static sc_status create_account_postgresql(PGconn *handle, const bc_new_account *account,
                                           bc_create_account_result *out, char *error,
                                           size_t error_size)
{
    char community[24];
    char created[BC_TIMESTAMP_TEXT_MAX];
    char code[24];
    char user_id[24];
    char contact_id[24];
    const char *params[7];
    PGresult *result;

    (void)snprintf(community, sizeof(community), "%llu", (unsigned long long)account->community_id);
    (void)snprintf(code, sizeof(code), "%llu",
                   (unsigned long long)account->email_verification_code);
    bc_sql_timestamp_text(account->created_at, created, sizeof(created));

    params[0] = account->gradido_id;
    params[1] = community;
    params[2] = account->first_name;
    params[3] = account->last_name;
    params[4] = account->language;
    params[5] = created;
    result = PQexecParams(handle,
                          "INSERT INTO users (gradido_id, community_id, first_name, last_name, "
                          "language, created_at) VALUES ($1, $2, $3, $4, $5, $6) RETURNING id",
                          6, NULL, params, NULL, NULL, 0);
    if (result == NULL || PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) != 1)
        return pg_refused(result, handle, out, error, error_size);
    (void)snprintf(user_id, sizeof(user_id), "%s", PQgetvalue(result, 0, 0));
    PQclear(result);

    params[0] = user_id;
    params[1] = BC_CONTACT_TYPE_EMAIL;
    params[2] = account->email;
    params[3] = code;
    params[4] = "1"; /* BC_OPT_IN_REGISTER */
    params[5] = created;
    result = PQexecParams(handle,
                          "INSERT INTO user_contacts (user_id, type, email, email_checked, "
                          "email_verification_code, email_opt_in_type_id, created_at) "
                          "VALUES ($1, $2, $3, false, $4, $5, $6) "
                          "ON CONFLICT (email) DO NOTHING RETURNING id",
                          6, NULL, params, NULL, NULL, 0);
    if (result == NULL || PQresultStatus(result) != PGRES_TUPLES_OK)
        return pg_refused(result, handle, out, error, error_size);
    if (PQntuples(result) == 0) {
        /* Not a failure: DO NOTHING did nothing, which is what the address being taken looks
         * like. The member row written a moment ago goes away with the rollback above this. */
        PQclear(result);
        out->outcome = BC_ACCOUNT_ADDRESS_TAKEN;
        return address_owner_postgresql(handle, account->email, &out->taken_by, error, error_size);
    }
    (void)snprintf(contact_id, sizeof(contact_id), "%s", PQgetvalue(result, 0, 0));
    PQclear(result);

    params[0] = contact_id;
    params[1] = user_id;
    result = PQexecParams(handle, "UPDATE users SET email_id = $1 WHERE id = $2", 2, NULL, params,
                          NULL, NULL, 0);
    if (result == NULL || PQresultStatus(result) != PGRES_COMMAND_OK)
        return pg_refused(result, handle, out, error, error_size);
    PQclear(result);

    out->outcome = BC_ACCOUNT_CREATED;
    out->user_id = strtoull(user_id, NULL, 10);
    return SC_OK;
}
#endif

sc_status bc_user_create_account(sc_db *db, const bc_new_account *account,
                                 bc_create_account_result *out, char *error, size_t error_size)
{
    char ignored[BC_SQL_ERROR_MAX];
    sc_status status;

    if (db == NULL || account == NULL || out == NULL || error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;
    error[0] = '\0';
    memset(out, 0, sizeof(*out));

    status = bc_sql_exec(db, "BEGIN", error, error_size);
    if (status != SC_OK)
        return status;

    switch (sc_db_kind_of(db)) {
    case SC_DB_SQLITE:
#if defined(SC_DB_WITH_SQLITE)
        status =
            create_account_sqlite((sqlite3 *)sc_db_native(db), account, out, error, error_size);
#else
        bc_sql_set_error(error, error_size, "this build has no SQLite driver");
        status = SC_ERR_UNAVAILABLE;
#endif
        break;
    case SC_DB_POSTGRESQL:
    default:
#if defined(SC_DB_WITH_POSTGRESQL)
        status =
            create_account_postgresql((PGconn *)sc_db_native(db), account, out, error, error_size);
#else
        bc_sql_set_error(error, error_size, "this build has no PostgreSQL driver");
        status = SC_ERR_UNAVAILABLE;
#endif
        break;
    }

    /* Only an account that was written commits. The other two outcomes take the member row back
     * out with them -- the identity sequence does not come back with it, which is a gap in
     * users.id and nothing more. */
    if (status != SC_OK || out->outcome != BC_ACCOUNT_CREATED) {
        (void)bc_sql_exec(db, "ROLLBACK", ignored, sizeof(ignored));
        return status;
    }
    return bc_sql_exec(db, "COMMIT", error, error_size);
}
