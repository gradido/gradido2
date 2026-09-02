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

sc_status bc_user_gradido_id_exists(sc_db *db, const char *gradido_id, uint64_t community_id,
                                    int *exists, char *error, size_t error_size)
{
    if (db == NULL || gradido_id == NULL || exists == NULL || error == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    *exists = 0;
    error[0] = '\0';

    switch (sc_db_kind_of(db)) {
    case SC_DB_SQLITE: {
#if defined(SC_DB_WITH_SQLITE)
        sqlite3 *handle = (sqlite3 *)sc_db_native(db);
        sqlite3_stmt *statement = NULL;
        sc_status status = SC_OK;
        int step;

        if (sqlite3_prepare_v2(handle,
                               "SELECT id FROM users WHERE gradido_id = ? AND community_id = ? "
                               "LIMIT 1",
                               -1, &statement, NULL) != SQLITE_OK) {
            bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
            return SC_ERR_INVALID_ARGUMENT;
        }
        sqlite3_bind_text(statement, 1, gradido_id, -1, SQLITE_STATIC);
        sqlite3_bind_int64(statement, 2, (sqlite3_int64)community_id);
        step = sqlite3_step(statement);
        if (step == SQLITE_ROW)
            *exists = 1;
        else if (step != SQLITE_DONE) {
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
        char community[24];
        const char *params[2];
        PGresult *result;
        sc_status status = SC_OK;

        (void)snprintf(community, sizeof(community), "%llu", (unsigned long long)community_id);
        params[0] = gradido_id;
        params[1] = community;
        result = PQexecParams(handle,
                              "SELECT id FROM users WHERE gradido_id = $1 AND community_id = $2 "
                              "LIMIT 1",
                              2, NULL, params, NULL, NULL, 0);
        if (result == NULL || PQresultStatus(result) != PGRES_TUPLES_OK) {
            bc_sql_set_error(error, error_size,
                             result != NULL ? PQresultErrorMessage(result)
                                            : PQerrorMessage(handle));
            status = SC_ERR_INVALID_ARGUMENT;
        } else if (PQntuples(result) > 0) {
            *exists = 1;
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
static sc_status create_account_sqlite(sqlite3 *handle, const bc_new_account *account,
                                       uint64_t *id_out, char *error, size_t error_size)
{
    sqlite3_stmt *statement = NULL;
    sqlite3_int64 user_id;
    sqlite3_int64 contact_id;

    if (sqlite3_prepare_v2(handle,
                           "INSERT INTO users (gradido_id, community_id, first_name, last_name, "
                           "language, created_at) VALUES (?, ?, ?, ?, ?, ?)",
                           -1, &statement, NULL) != SQLITE_OK)
        goto failed;
    sqlite3_bind_text(statement, 1, account->gradido_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(statement, 2, (sqlite3_int64)account->community_id);
    sqlite3_bind_text(statement, 3, account->first_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 4, account->last_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 5, account->language, -1, SQLITE_STATIC);
    sqlite3_bind_int64(statement, 6, account->created_at);
    if (sqlite3_step(statement) != SQLITE_DONE)
        goto failed;
    sqlite3_finalize(statement);
    statement = NULL;
    user_id = sqlite3_last_insert_rowid(handle);

    if (sqlite3_prepare_v2(handle,
                           "INSERT INTO user_contacts (user_id, type, email, email_checked, "
                           "email_verification_code, email_opt_in_type_id, created_at) "
                           "VALUES (?, ?, ?, 0, ?, ?, ?)",
                           -1, &statement, NULL) != SQLITE_OK)
        goto failed;
    sqlite3_bind_int64(statement, 1, user_id);
    sqlite3_bind_text(statement, 2, BC_CONTACT_TYPE_EMAIL, -1, SQLITE_STATIC);
    sqlite3_bind_text(statement, 3, account->email, -1, SQLITE_STATIC);
    sqlite3_bind_int64(statement, 4, (sqlite3_int64)account->email_verification_code);
    sqlite3_bind_int(statement, 5, BC_OPT_IN_REGISTER);
    sqlite3_bind_int64(statement, 6, account->created_at);
    if (sqlite3_step(statement) != SQLITE_DONE)
        goto failed;
    sqlite3_finalize(statement);
    statement = NULL;
    contact_id = sqlite3_last_insert_rowid(handle);

    if (sqlite3_prepare_v2(handle, "UPDATE users SET email_id = ? WHERE id = ?", -1, &statement,
                           NULL) != SQLITE_OK)
        goto failed;
    sqlite3_bind_int64(statement, 1, contact_id);
    sqlite3_bind_int64(statement, 2, user_id);
    if (sqlite3_step(statement) != SQLITE_DONE)
        goto failed;
    sqlite3_finalize(statement);

    *id_out = (uint64_t)user_id;
    return SC_OK;

failed:
    bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
    sqlite3_finalize(statement);
    return SC_ERR_INVALID_ARGUMENT;
}
#endif

#if defined(SC_DB_WITH_POSTGRESQL)
static sc_status create_account_postgresql(PGconn *handle, const bc_new_account *account,
                                           uint64_t *id_out, char *error, size_t error_size)
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
    if (result == NULL || PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) != 1) {
        bc_sql_set_error(error, error_size,
                         result != NULL ? PQresultErrorMessage(result) : PQerrorMessage(handle));
        PQclear(result);
        return SC_ERR_INVALID_ARGUMENT;
    }
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
                          "VALUES ($1, $2, $3, false, $4, $5, $6) RETURNING id",
                          6, NULL, params, NULL, NULL, 0);
    if (result == NULL || PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) != 1) {
        bc_sql_set_error(error, error_size,
                         result != NULL ? PQresultErrorMessage(result) : PQerrorMessage(handle));
        PQclear(result);
        return SC_ERR_INVALID_ARGUMENT;
    }
    (void)snprintf(contact_id, sizeof(contact_id), "%s", PQgetvalue(result, 0, 0));
    PQclear(result);

    params[0] = contact_id;
    params[1] = user_id;
    result = PQexecParams(handle, "UPDATE users SET email_id = $1 WHERE id = $2", 2, NULL, params,
                          NULL, NULL, 0);
    if (result == NULL || PQresultStatus(result) != PGRES_COMMAND_OK) {
        bc_sql_set_error(error, error_size,
                         result != NULL ? PQresultErrorMessage(result) : PQerrorMessage(handle));
        PQclear(result);
        return SC_ERR_INVALID_ARGUMENT;
    }
    PQclear(result);

    *id_out = strtoull(user_id, NULL, 10);
    return SC_OK;
}
#endif

sc_status bc_user_create_account(sc_db *db, const bc_new_account *account, uint64_t *id_out,
                                 char *error, size_t error_size)
{
    char ignored[BC_SQL_ERROR_MAX];
    sc_status status;

    if (db == NULL || account == NULL || id_out == NULL || error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;
    error[0] = '\0';

    status = bc_sql_exec(db, "BEGIN", error, error_size);
    if (status != SC_OK)
        return status;

    switch (sc_db_kind_of(db)) {
    case SC_DB_SQLITE:
#if defined(SC_DB_WITH_SQLITE)
        status =
            create_account_sqlite((sqlite3 *)sc_db_native(db), account, id_out, error, error_size);
#else
        bc_sql_set_error(error, error_size, "this build has no SQLite driver");
        status = SC_ERR_UNAVAILABLE;
#endif
        break;
    case SC_DB_POSTGRESQL:
    default:
#if defined(SC_DB_WITH_POSTGRESQL)
        status = create_account_postgresql((PGconn *)sc_db_native(db), account, id_out, error,
                                           error_size);
#else
        bc_sql_set_error(error, error_size, "this build has no PostgreSQL driver");
        status = SC_ERR_UNAVAILABLE;
#endif
        break;
    }

    if (status != SC_OK) {
        (void)bc_sql_exec(db, "ROLLBACK", ignored, sizeof(ignored));
        return status;
    }
    return bc_sql_exec(db, "COMMIT", error, error_size);
}
