/*
 * How the community rows are loaded and persisted. The interaction decides *when*.
 *
 * Only the home community so far: every other row arrives through federation, which does not
 * exist yet. Both statements here are startup-only, which is why neither is on a hot path and
 * why neither caches anything -- the caller holds the result for the life of the process.
 *
 * Every statement is written twice, once per dialect, and that is the design rather than a wart:
 * Architecture.md, *Databases*, has no query surface both drivers implement precisely so that a
 * repository has to say which database it is talking to. What differs between the two branches
 * is only the placeholder syntax, how a boolean is spelled and how bytes are passed; the shape
 * of the statement is the same, which is what makes the pair reviewable.
 */
#include "backend_core/domain/community.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "service_core/log.h"

#if defined(SC_DB_WITH_SQLITE)
#include <sqlite3.h>
#endif
#if defined(SC_DB_WITH_POSTGRESQL)
#include <libpq-fe.h>
#endif

/* LIMIT 2 rather than 1: one row is the answer, two is a broken database, and asking for two is
 * what tells them apart. */
static const char kSelectHomeSqlite[] =
    "SELECT id, community_uuid, url, name, description, public_key "
    "FROM communities WHERE remote = 0 LIMIT 2";
static const char kSelectHomePostgresql[] =
    "SELECT id, community_uuid, url, name, description, public_key "
    "FROM communities WHERE remote = false LIMIT 2";

/** Copies @p text into @p out, refusing a value longer than the column it came from. */
static int copy_column(char *out, size_t out_size, const char *text)
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

sc_status bc_community_find_home(sc_db *db, bc_home_community *out, int *found, char *error,
                                 size_t error_size)
{
    if (db == NULL || out == NULL || found == NULL || error == NULL || error_size == 0)
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

        if (sqlite3_prepare_v2(handle, kSelectHomeSqlite, -1, &statement, NULL) != SQLITE_OK) {
            bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
            return SC_ERR_INVALID_ARGUMENT;
        }
        step = sqlite3_step(statement);
        if (step == SQLITE_ROW) {
            const void *key = sqlite3_column_blob(statement, 5);
            int key_size = sqlite3_column_bytes(statement, 5);

            out->id = (uint64_t)sqlite3_column_int64(statement, 0);
            if (!copy_column(out->community_uuid, sizeof(out->community_uuid),
                             (const char *)sqlite3_column_text(statement, 1)) ||
                !copy_column(out->url, sizeof(out->url),
                             (const char *)sqlite3_column_text(statement, 2)) ||
                !copy_column(out->name, sizeof(out->name),
                             (const char *)sqlite3_column_text(statement, 3)) ||
                key_size != BC_PUBLIC_KEY_SIZE) {
                bc_sql_set_error(error, error_size,
                                 "the home community row does not fit the contracted columns");
                status = SC_ERR_MALFORMED;
            } else {
                out->has_description = sqlite3_column_type(statement, 4) != SQLITE_NULL;
                if (out->has_description &&
                    !copy_column(out->description, sizeof(out->description),
                                 (const char *)sqlite3_column_text(statement, 4))) {
                    bc_sql_set_error(error, error_size, "the community description is too long");
                    status = SC_ERR_MALFORMED;
                } else {
                    memcpy(out->public_key, key, BC_PUBLIC_KEY_SIZE);
                    *found = 1;
                }
            }
            if (status == SC_OK && sqlite3_step(statement) == SQLITE_ROW) {
                bc_sql_set_error(error, error_size,
                                 "more than one home community: communities.remote = false on "
                                 "several rows");
                status = SC_ERR_MALFORMED;
                *found = 0;
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
        PGresult *result = PQexec(handle, kSelectHomePostgresql);
        sc_status status = SC_OK;

        if (result == NULL || PQresultStatus(result) != PGRES_TUPLES_OK) {
            bc_sql_set_error(error, error_size,
                             result != NULL ? PQresultErrorMessage(result)
                                            : PQerrorMessage(handle));
            PQclear(result);
            return SC_ERR_INVALID_ARGUMENT;
        }
        if (PQntuples(result) > 1) {
            bc_sql_set_error(error, error_size,
                             "more than one home community: communities.remote = false on several "
                             "rows");
            status = SC_ERR_MALFORMED;
        } else if (PQntuples(result) == 1) {
            out->id = strtoull(PQgetvalue(result, 0, 0), NULL, 10);
            out->has_description = !PQgetisnull(result, 0, 4);
            if (!copy_column(out->community_uuid, sizeof(out->community_uuid),
                             PQgetvalue(result, 0, 1)) ||
                !copy_column(out->url, sizeof(out->url), PQgetvalue(result, 0, 2)) ||
                !copy_column(out->name, sizeof(out->name),
                             PQgetisnull(result, 0, 3) ? "" : PQgetvalue(result, 0, 3)) ||
                (out->has_description && !copy_column(out->description, sizeof(out->description),
                                                      PQgetvalue(result, 0, 4)))) {
                bc_sql_set_error(error, error_size,
                                 "the home community row does not fit the contracted columns");
                status = SC_ERR_MALFORMED;
            } else if (bc_sql_bytea_parse(PQgetvalue(result, 0, 5), out->public_key,
                                          BC_PUBLIC_KEY_SIZE) != BC_PUBLIC_KEY_SIZE) {
                bc_sql_set_error(error, error_size,
                                 "communities.public_key is not 32 bytes on the home community");
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

/** Writes the home community. Called once, at first start, and never again. */
static sc_status insert_home(sc_db *db, const bc_home_community_setup *setup, const char *uuid,
                             const uint8_t *public_key, const uint8_t *private_key,
                             int64_t created_at, uint64_t *id_out, char *error, size_t error_size)
{
    const char *description = setup->has_description ? setup->description : NULL;

    switch (sc_db_kind_of(db)) {
    case SC_DB_SQLITE: {
#if defined(SC_DB_WITH_SQLITE)
        sqlite3 *handle = (sqlite3 *)sc_db_native(db);
        sqlite3_stmt *statement = NULL;
        int step;

        /* creation_date is when the community was founded, as far as this instance knows: now.
         * Distinct from created_at, which is when this row was written -- the two coincide only
         * here. */
        if (sqlite3_prepare_v2(handle,
                               "INSERT INTO communities (remote, url, public_key, private_key, "
                               "community_uuid, name, description, creation_date, created_at) "
                               "VALUES (0, ?, ?, ?, ?, ?, ?, ?, ?)",
                               -1, &statement, NULL) != SQLITE_OK) {
            bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
            return SC_ERR_INVALID_ARGUMENT;
        }
        sqlite3_bind_text(statement, 1, setup->url, -1, SQLITE_STATIC);
        sqlite3_bind_blob(statement, 2, public_key, BC_PUBLIC_KEY_SIZE, SQLITE_STATIC);
        sqlite3_bind_blob(statement, 3, private_key, BC_PRIVATE_KEY_SIZE, SQLITE_STATIC);
        sqlite3_bind_text(statement, 4, uuid, -1, SQLITE_STATIC);
        sqlite3_bind_text(statement, 5, setup->name, -1, SQLITE_STATIC);
        if (description != NULL)
            sqlite3_bind_text(statement, 6, description, -1, SQLITE_STATIC);
        else
            sqlite3_bind_null(statement, 6);
        sqlite3_bind_int64(statement, 7, created_at);
        sqlite3_bind_int64(statement, 8, created_at);
        step = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (step != SQLITE_DONE) {
            bc_sql_set_error(error, error_size, sqlite3_errmsg(handle));
            return SC_ERR_INVALID_ARGUMENT;
        }
        *id_out = (uint64_t)sqlite3_last_insert_rowid(handle);
        return SC_OK;
#else
        bc_sql_set_error(error, error_size, "this build has no SQLite driver");
        return SC_ERR_UNAVAILABLE;
#endif
    }
    case SC_DB_POSTGRESQL:
    default: {
#if defined(SC_DB_WITH_POSTGRESQL)
        PGconn *handle = (PGconn *)sc_db_native(db);
        char public_text[BC_PUBLIC_KEY_SIZE * 2 + 3];
        char private_text[BC_PRIVATE_KEY_SIZE * 2 + 3];
        char created[BC_TIMESTAMP_TEXT_MAX];
        const char *params[8];
        PGresult *result;
        int ok;

        if (!bc_sql_bytea_text(public_key, BC_PUBLIC_KEY_SIZE, public_text, sizeof(public_text)) ||
            !bc_sql_bytea_text(private_key, BC_PRIVATE_KEY_SIZE, private_text,
                               sizeof(private_text))) {
            bc_sql_set_error(error, error_size, "the community key pair does not fit its columns");
            return SC_ERR_TOO_LONG;
        }
        bc_sql_timestamp_text(created_at, created, sizeof(created));
        params[0] = setup->url;
        params[1] = public_text;
        params[2] = private_text;
        params[3] = uuid;
        params[4] = setup->name;
        params[5] = description;
        params[6] = created;
        params[7] = created;
        result = PQexecParams(handle,
                              "INSERT INTO communities (remote, url, public_key, private_key, "
                              "community_uuid, name, description, creation_date, created_at) "
                              "VALUES (false, $1, $2, $3, $4, $5, $6, $7, $8) RETURNING id",
                              8, NULL, params, NULL, NULL, 0);
        ok = result != NULL && PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) == 1;
        if (!ok) {
            bc_sql_set_error(error, error_size,
                             result != NULL ? PQresultErrorMessage(result)
                                            : PQerrorMessage(handle));
            PQclear(result);
            return SC_ERR_INVALID_ARGUMENT;
        }
        *id_out = strtoull(PQgetvalue(result, 0, 0), NULL, 10);
        PQclear(result);
        return SC_OK;
#else
        bc_sql_set_error(error, error_size, "this build has no PostgreSQL driver");
        return SC_ERR_UNAVAILABLE;
#endif
    }
    }
}

sc_status bc_create_home_community(sc_db *db, const bc_home_community_setup *setup,
                                   bc_home_community *out, char *error, size_t error_size)
{
    uint8_t private_key[BC_PRIVATE_KEY_SIZE];
    int64_t created_at = sc_now_ms();
    sc_status status;

    if (db == NULL || setup == NULL || out == NULL || error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    /* Not a draw-and-check like users.gradido_id: communities_uuid_key is a plain unique index on
     * one column, so the database is the check. Legacy loops here because its equivalent index is
     * the same shape and it chose to look first anyway. */
    bc_new_uuid(out->community_uuid);
    bc_community_new_keys(out->public_key, private_key);

    status = insert_home(db, setup, out->community_uuid, out->public_key, private_key, created_at,
                         &out->id, error, error_size);
    /* The private key leaves this frame nowhere else: it is written and forgotten, and the
     * community the caller gets does not carry it. */
    memset(private_key, 0, sizeof(private_key));
    if (status != SC_OK)
        return status;

    (void)snprintf(out->url, sizeof(out->url), "%s", setup->url);
    (void)snprintf(out->name, sizeof(out->name), "%s", setup->name);
    out->has_description = setup->has_description;
    if (setup->has_description)
        (void)snprintf(out->description, sizeof(out->description), "%s", setup->description);

    {
        sc_log_value data[2] = {SC_LOG_STR("uuid", out->community_uuid),
                                SC_LOG_STR("url", out->url)};
        sc_log_context context = {0};

        context.data = data;
        context.data_count = 2;
        sc_log_event(SC_LOG_INFO, SC_CAT_COMMUNITY, "community.home.created", &context,
                     "home community \"%s\" created", out->name);
    }
    return SC_OK;
}
