/*
 * Statements on SQLite.
 *
 * Each statement is prepared once per connection with SQLITE_PREPARE_PERSISTENT -- the hint
 * that it will be used again and belongs in memory that does not churn -- and kept in the
 * connection's table. Running it is reset, bind, step; closing it is reset again. Nothing is
 * finalized until the connection is.
 *
 * Every value binds as what it is: an int64, a text, a blob. SQLITE_STATIC throughout, which is
 * why the parameters must outlive the cursor (sql.h): SQLite reads them at each step, not only
 * at the first.
 */
#include "service_core/sql.h"

#include "db_internal.h"

#if defined(SC_DB_WITH_SQLITE)

#include <stdio.h>
#include <string.h>

#include <sqlite3.h>

static sc_status refused(sc_db *db, sqlite3_stmt *statement, sc_sql_error *error)
{
    sqlite3 *handle = (sqlite3 *)db->native;
    const int code = sqlite3_extended_errcode(handle);
    const char *said = sqlite3_errmsg(handle);
    sc_sql_error_kind kind = SC_SQL_ERROR_OTHER;

    if (code == SQLITE_CONSTRAINT_UNIQUE || code == SQLITE_CONSTRAINT_PRIMARYKEY)
        kind = SC_SQL_ERROR_UNIQUE;
    else if (code == SQLITE_CONSTRAINT_FOREIGNKEY)
        kind = SC_SQL_ERROR_FOREIGN_KEY;
    else if (code == SQLITE_CONSTRAINT_NOTNULL)
        kind = SC_SQL_ERROR_NOT_NULL;
    sc_sql_set_error(error, kind, said);

    /* SQLite has no constraint names to give; the columns it lists after this prefix are the
     * nearest thing, and a repository matches a column name that both spellings contain. */
    if (error != NULL) {
        static const char kPrefix[] = "UNIQUE constraint failed: ";
        const char *at = kind == SC_SQL_ERROR_UNIQUE && said != NULL ? strstr(said, kPrefix) : NULL;

        (void)snprintf(error->constraint, sizeof(error->constraint), "%s",
                       at != NULL ? at + sizeof(kPrefix) - 1 : "");
    }
    if (statement != NULL) {
        (void)sqlite3_reset(statement);
        (void)sqlite3_clear_bindings(statement);
    }
    /* SQLite has no connection to lose; a refusal is a refusal. */
    return SC_ERR_INVALID_ARGUMENT;
}

static sc_status bind(sc_db *db, sqlite3_stmt *statement, const sc_sql_param *params,
                      uint32_t param_count, sc_sql_error *error)
{
    uint32_t i;

    for (i = 0; i != param_count; ++i) {
        const sc_sql_param *p = &params[i];
        const int at = (int)i + 1;
        int rc;

        switch (p->type) {
        case SC_SQL_INT:
        case SC_SQL_TIME:
            rc = sqlite3_bind_int64(statement, at, p->number);
            break;
        case SC_SQL_BOOL:
            rc = sqlite3_bind_int(statement, at, p->number != 0);
            break;
        case SC_SQL_TEXT:
            rc = sqlite3_bind_text(statement, at, (const char *)p->data, (int)p->size,
                                   SQLITE_STATIC);
            break;
        case SC_SQL_BYTES:
            rc = sqlite3_bind_blob(statement, at, p->data, (int)p->size, SQLITE_STATIC);
            break;
        case SC_SQL_NULL:
        default:
            rc = sqlite3_bind_null(statement, at);
            break;
        }
        if (rc != SQLITE_OK)
            return refused(db, statement, error);
    }
    return SC_OK;
}

sc_status sc_sql_sqlite_run(sc_db *db, sc_sql_statement *statement, int32_t slot,
                            const sc_sql_param *params, uint32_t param_count, sc_sql_rows *rows,
                            int64_t *changes, sc_sql_error *error)
{
    sqlite3 *handle = (sqlite3 *)db->native;
    sqlite3_stmt *prepared = (sqlite3_stmt *)db->prepared[slot];
    sc_status status;
    int step;

    if (statement->sqlite == NULL) {
        sc_sql_set_error(error, SC_SQL_ERROR_OTHER, "this statement has no SQLite text");
        return SC_ERR_INVALID_ARGUMENT;
    }
    if (prepared == NULL) {
        if (sqlite3_prepare_v3(handle, statement->sqlite, -1, SQLITE_PREPARE_PERSISTENT, &prepared,
                               NULL) != SQLITE_OK)
            return refused(db, NULL, error);
        db->prepared[slot] = prepared;
    }

    /* A cursor somebody left open on this statement is ended here rather than bound into:
     * binding a statement mid-step is SQLITE_MISUSE. */
    if (sqlite3_stmt_busy(prepared))
        (void)sqlite3_reset(prepared);
    status = bind(db, prepared, params, param_count, error);
    if (status != SC_OK)
        return status;

    /* The first step runs the statement: an INSERT has written its row and its constraints have
     * been checked by the time this returns, whatever the caller does with the rows. */
    step = sqlite3_step(prepared);
    if (step != SQLITE_ROW && step != SQLITE_DONE)
        return refused(db, prepared, error);

    if (changes != NULL)
        *changes = sqlite3_changes64(handle);
    if (rows == NULL) {
        (void)sqlite3_reset(prepared);
        (void)sqlite3_clear_bindings(prepared);
        return SC_OK;
    }
    rows->db = db;
    rows->handle = prepared;
    rows->pending = step;
    return SC_OK;
}

int sc_sql_sqlite_next(sc_sql_rows *rows)
{
    sqlite3_stmt *statement = (sqlite3_stmt *)rows->handle;

    /* The row sc_sql_sqlite_run already stepped to is handed out first; after that, a step per
     * row. An error in the middle of a result ends it like its last row would. */
    if (rows->pending != 0) {
        const int had = rows->pending;

        rows->pending = 0;
        return had == SQLITE_ROW;
    }
    return sqlite3_step(statement) == SQLITE_ROW;
}

void sc_sql_sqlite_close(sc_sql_rows *rows)
{
    sqlite3_stmt *statement = (sqlite3_stmt *)rows->handle;

    (void)sqlite3_reset(statement);
    (void)sqlite3_clear_bindings(statement);
}

sc_status sc_sql_sqlite_simple(sc_db *db, const char *text, sc_sql_error *error)
{
    char *message = NULL;

    if (sqlite3_exec((sqlite3 *)db->native, text, NULL, NULL, &message) != SQLITE_OK) {
        sqlite3_free(message);
        return refused(db, NULL, error);
    }
    sqlite3_free(message);
    return SC_OK;
}

int sc_sql_sqlite_is_null(const sc_sql_rows *rows, uint32_t column)
{
    return sqlite3_column_type((sqlite3_stmt *)rows->handle, (int)column) == SQLITE_NULL;
}

int64_t sc_sql_sqlite_int(const sc_sql_rows *rows, uint32_t column)
{
    return sqlite3_column_int64((sqlite3_stmt *)rows->handle, (int)column);
}

const char *sc_sql_sqlite_text(const sc_sql_rows *rows, uint32_t column, uint32_t *size)
{
    sqlite3_stmt *statement = (sqlite3_stmt *)rows->handle;
    const char *text;

    if (sqlite3_column_type(statement, (int)column) == SQLITE_NULL)
        return NULL;
    /* text first, bytes second: that is the order SQLite's documentation gives for a
     * conversion to happen before its size is asked. */
    text = (const char *)sqlite3_column_text(statement, (int)column);
    if (size != NULL)
        *size = (uint32_t)sqlite3_column_bytes(statement, (int)column);
    return text;
}

int64_t sc_sql_sqlite_bytes(const sc_sql_rows *rows, uint32_t column, uint8_t *out, size_t out_size)
{
    sqlite3_stmt *statement = (sqlite3_stmt *)rows->handle;
    const void *blob;
    int size;

    if (sqlite3_column_type(statement, (int)column) != SQLITE_BLOB)
        return -1;
    blob = sqlite3_column_blob(statement, (int)column);
    size = sqlite3_column_bytes(statement, (int)column);
    if (size < 0 || (size_t)size > out_size)
        return -1;
    if (size > 0)
        memcpy(out, blob, (size_t)size);
    return size;
}

void sc_sql_sqlite_forget(sc_db *db)
{
    size_t i;

    for (i = 0; i != SC_SQL_STATEMENTS_MAX; ++i) {
        if (db->prepared[i] != NULL) {
            (void)sqlite3_finalize((sqlite3_stmt *)db->prepared[i]);
            db->prepared[i] = NULL;
        }
    }
}

#else /* the build was told to leave this driver out */

static sc_status absent(sc_sql_error *error)
{
    sc_sql_set_error(error, SC_SQL_ERROR_OTHER, "this build has no SQLite driver");
    return SC_ERR_UNAVAILABLE;
}

sc_status sc_sql_sqlite_run(sc_db *db, sc_sql_statement *statement, int32_t slot,
                            const sc_sql_param *params, uint32_t param_count, sc_sql_rows *rows,
                            int64_t *changes, sc_sql_error *error)
{
    (void)db, (void)statement, (void)slot, (void)params, (void)param_count, (void)rows;
    (void)changes;
    return absent(error);
}

int sc_sql_sqlite_next(sc_sql_rows *rows)
{
    (void)rows;
    return 0;
}

void sc_sql_sqlite_close(sc_sql_rows *rows)
{
    (void)rows;
}

sc_status sc_sql_sqlite_simple(sc_db *db, const char *text, sc_sql_error *error)
{
    (void)db, (void)text;
    return absent(error);
}

int sc_sql_sqlite_is_null(const sc_sql_rows *rows, uint32_t column)
{
    (void)rows, (void)column;
    return 1;
}

int64_t sc_sql_sqlite_int(const sc_sql_rows *rows, uint32_t column)
{
    (void)rows, (void)column;
    return 0;
}

const char *sc_sql_sqlite_text(const sc_sql_rows *rows, uint32_t column, uint32_t *size)
{
    (void)rows, (void)column, (void)size;
    return NULL;
}

int64_t sc_sql_sqlite_bytes(const sc_sql_rows *rows, uint32_t column, uint8_t *out, size_t out_size)
{
    (void)rows, (void)column, (void)out, (void)out_size;
    return -1;
}

void sc_sql_sqlite_forget(sc_db *db)
{
    (void)db;
}

#endif /* SC_DB_WITH_SQLITE */
