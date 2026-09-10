/*
 * Statements on PostgreSQL, through libpq.
 *
 * Each statement is prepared once per session, under a name that is its slot -- `s12` -- and
 * then only executed: PQprepare parses and plans, PQexecPrepared binds and runs, and a
 * statement run a million times is parsed once per connection rather than a million times.
 *
 * Parameters go as text, except bytes, which go as binary: bytea's binary form is the bytes
 * themselves, where its text form would be a hex encoding done here and undone on the server.
 * Everything else stays text because the server infers each parameter's type from where it is
 * used, and a binary uuid or timestamptz would have to be encoded the way that type wants --
 * text is the one format every type reads. Results come back as text for the reason sql.c
 * gives.
 */
#include "service_core/sql.h"

#include "db_internal.h"

#if defined(SC_DB_WITH_POSTGRESQL)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libpq-fe.h>

/*
 * Where a text parameter that is not NUL-terminated is copied to, because the text format needs
 * the terminator. Per thread, because a connection is used by one thread at a time and so is
 * the statement being bound; one statement's copies together may not exceed it.
 */
#define TEXT_SCRATCH_BYTES (64u * 1024u)
static _Thread_local char t_text_scratch[TEXT_SCRATCH_BYTES];

/* `2026-09-10T12:34:56.789Z`, which timestamptz reads as UTC whatever the session says. */
static void timestamp_text(int64_t unix_ms, char *out, size_t out_size)
{
    time_t seconds = (time_t)(unix_ms / 1000);
    int millis = (int)(unix_ms % 1000);
    struct tm utc;

    if (millis < 0) {
        millis += 1000;
        seconds -= 1;
    }
    if (gmtime_r(&seconds, &utc) == NULL) {
        out[0] = '\0';
        return;
    }
    (void)snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                   utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
}

/* A refusal, read out of the result the way both callers need it: the SQLSTATE decides the
 * kind, and a connection libpq now calls bad is a connection, whatever else it said. */
static sc_status refused(sc_db *db, PGresult *result, sc_sql_error *error)
{
    PGconn *conn = (PGconn *)db->native;
    const char *state = result != NULL ? PQresultErrorField(result, PG_DIAG_SQLSTATE) : NULL;
    const char *constraint =
        result != NULL ? PQresultErrorField(result, PG_DIAG_CONSTRAINT_NAME) : NULL;
    sc_sql_error_kind kind = SC_SQL_ERROR_OTHER;

    if (PQstatus(conn) == CONNECTION_BAD)
        kind = SC_SQL_ERROR_CONNECTION;
    else if (state != NULL && strcmp(state, "23505") == 0)
        kind = SC_SQL_ERROR_UNIQUE;
    else if (state != NULL && strcmp(state, "23503") == 0)
        kind = SC_SQL_ERROR_FOREIGN_KEY;
    else if (state != NULL && strcmp(state, "23502") == 0)
        kind = SC_SQL_ERROR_NOT_NULL;

    sc_sql_set_error(error, kind,
                     result != NULL ? PQresultErrorMessage(result) : PQerrorMessage(conn));
    if (error != NULL)
        (void)snprintf(error->constraint, sizeof(error->constraint), "%s",
                       constraint != NULL ? constraint : "");
    PQclear(result);
    return kind == SC_SQL_ERROR_CONNECTION ? SC_ERR_NETWORK : SC_ERR_INVALID_ARGUMENT;
}

/* 26000, invalid_sql_statement_name: this session has no statement by that name. */
static int is_unknown_statement(const PGresult *result)
{
    const char *state = result != NULL ? PQresultErrorField(result, PG_DIAG_SQLSTATE) : NULL;

    return state != NULL && strcmp(state, "26000") == 0;
}

static sc_status prepare(sc_db *db, sc_sql_statement *statement, int32_t slot, const char *name,
                         sc_sql_error *error)
{
    PGresult *result;

    if (db->prepared[slot] != NULL)
        return SC_OK;
    result = PQprepare((PGconn *)db->native, name, statement->postgresql, 0, NULL);
    if (result == NULL || PQresultStatus(result) != PGRES_COMMAND_OK)
        return refused(db, result, error);
    PQclear(result);
    db->prepared[slot] = db; /* any non-NULL marker: this session knows the name */
    return SC_OK;
}

sc_status sc_sql_postgres_run(sc_db *db, sc_sql_statement *statement, int32_t slot,
                              const sc_sql_param *params, uint32_t param_count, sc_sql_rows *rows,
                              int64_t *changes, sc_sql_error *error)
{
    const char *values[SC_SQL_PARAMS_MAX];
    int lengths[SC_SQL_PARAMS_MAX];
    int formats[SC_SQL_PARAMS_MAX];
    char numbers[SC_SQL_PARAMS_MAX][32];
    char name[16];
    size_t scratch_used = 0;
    PGresult *result;
    ExecStatusType outcome;
    uint32_t i;
    int attempt;
    sc_status status;

    if (statement->postgresql == NULL) {
        sc_sql_set_error(error, SC_SQL_ERROR_OTHER, "this statement has no PostgreSQL text");
        return SC_ERR_INVALID_ARGUMENT;
    }
    (void)snprintf(name, sizeof(name), "s%d", (int)slot);

    for (i = 0; i != param_count; ++i) {
        const sc_sql_param *p = &params[i];

        lengths[i] = 0;
        formats[i] = 0;
        switch (p->type) {
        case SC_SQL_NULL:
            values[i] = NULL;
            break;
        case SC_SQL_INT:
            (void)snprintf(numbers[i], sizeof(numbers[i]), "%lld", (long long)p->number);
            values[i] = numbers[i];
            break;
        case SC_SQL_BOOL:
            values[i] = p->number != 0 ? "true" : "false";
            break;
        case SC_SQL_TIME:
            timestamp_text(p->number, numbers[i], sizeof(numbers[i]));
            values[i] = numbers[i];
            break;
        case SC_SQL_BYTES:
            values[i] = (const char *)p->data;
            lengths[i] = (int)p->size;
            formats[i] = 1;
            break;
        case SC_SQL_TEXT:
        default:
            if (p->terminated) {
                values[i] = (const char *)p->data;
                break;
            }
            if (scratch_used + p->size + 1 > TEXT_SCRATCH_BYTES) {
                sc_sql_set_error(error, SC_SQL_ERROR_OTHER,
                                 "the text parameters of one statement exceed 64 KiB");
                return SC_ERR_TOO_LONG;
            }
            memcpy(t_text_scratch + scratch_used, p->data, p->size);
            t_text_scratch[scratch_used + p->size] = '\0';
            values[i] = t_text_scratch + scratch_used;
            scratch_used += p->size + 1;
            break;
        }
    }

    /*
     * A session that has lost a name this table still believes in -- a DISCARD ALL, a DEALLOCATE,
     * a pooler that handed this transaction to a server connection that never saw it. Only the
     * name that was reported missing is forgotten: whether the others went with it cannot be
     * seen from here, and assuming they did is worse than finding out -- preparing a name the
     * session still has is refused as a duplicate, and that slot would then never work on this
     * connection again. A session that lost everything pays one failure per statement instead,
     * each of them recovered the same way.
     *
     * What happens next depends on where the statement ran. Outside a transaction nothing is
     * broken but the name, and the statement is prepared again and run, once. Inside one,
     * PostgreSQL has already aborted the transaction: a second attempt here would only be
     * refused with 25P02, and every statement the unit ran before this one is gone with it. So
     * the statement fails, and the connection is marked for the executor, which rolls back and
     * runs the whole unit again -- this statement prepared afresh.
     */
    for (attempt = 0;; ++attempt) {
        status = prepare(db, statement, slot, name, error);
        if (status != SC_OK)
            return status;
        result = PQexecPrepared((PGconn *)db->native, name, (int)param_count, values, lengths,
                                formats, 0);
        if (!is_unknown_statement(result))
            break;
        db->prepared[slot] = NULL;
        if (attempt == 0 && PQtransactionStatus((PGconn *)db->native) == PQTRANS_IDLE) {
            PQclear(result);
            continue;
        }
        db->rerun_unit = 1;
        break;
    }

    outcome = result != NULL ? PQresultStatus(result) : PGRES_FATAL_ERROR;
    if (outcome != PGRES_TUPLES_OK && outcome != PGRES_COMMAND_OK)
        return refused(db, result, error);

    if (changes != NULL) {
        const char *affected = PQcmdTuples(result);

        *changes = affected != NULL && affected[0] != '\0' ? strtoll(affected, NULL, 10) : 0;
    }
    if (rows == NULL) {
        PQclear(result);
        return SC_OK;
    }
    rows->db = db;
    rows->handle = result;
    rows->row = -1;
    rows->count = PQntuples(result);
    return SC_OK;
}

int sc_sql_postgres_next(sc_sql_rows *rows)
{
    if (rows->row + 1 >= rows->count)
        return 0;
    ++rows->row;
    return 1;
}

void sc_sql_postgres_close(sc_sql_rows *rows)
{
    PQclear((PGresult *)rows->handle);
}

sc_status sc_sql_postgres_simple(sc_db *db, const char *text, sc_sql_error *error)
{
    PGresult *result = PQexec((PGconn *)db->native, text);
    ExecStatusType outcome = result != NULL ? PQresultStatus(result) : PGRES_FATAL_ERROR;

    if (outcome != PGRES_COMMAND_OK && outcome != PGRES_TUPLES_OK)
        return refused(db, result, error);
    PQclear(result);
    return SC_OK;
}

static int in_range(const sc_sql_rows *rows, uint32_t column)
{
    const PGresult *result = (const PGresult *)rows->handle;

    return rows->row >= 0 && rows->row < rows->count && (int)column < PQnfields(result);
}

int sc_sql_postgres_is_null(const sc_sql_rows *rows, uint32_t column)
{
    if (!in_range(rows, column))
        return 1;
    return PQgetisnull((const PGresult *)rows->handle, rows->row, (int)column);
}

const char *sc_sql_postgres_text(const sc_sql_rows *rows, uint32_t column, uint32_t *size)
{
    const PGresult *result = (const PGresult *)rows->handle;

    if (!in_range(rows, column) || PQgetisnull(result, rows->row, (int)column))
        return NULL;
    if (size != NULL)
        *size = (uint32_t)PQgetlength(result, rows->row, (int)column);
    return PQgetvalue(result, rows->row, (int)column);
}

void sc_sql_postgres_forget(sc_db *db)
{
    memset(db->prepared, 0, sizeof(db->prepared));
}

#else /* the build was told to leave this driver out */

static sc_status absent(sc_sql_error *error)
{
    sc_sql_set_error(error, SC_SQL_ERROR_OTHER, "this build has no PostgreSQL driver");
    return SC_ERR_UNAVAILABLE;
}

sc_status sc_sql_postgres_run(sc_db *db, sc_sql_statement *statement, int32_t slot,
                              const sc_sql_param *params, uint32_t param_count, sc_sql_rows *rows,
                              int64_t *changes, sc_sql_error *error)
{
    (void)db, (void)statement, (void)slot, (void)params, (void)param_count, (void)rows;
    (void)changes;
    return absent(error);
}

int sc_sql_postgres_next(sc_sql_rows *rows)
{
    (void)rows;
    return 0;
}

void sc_sql_postgres_close(sc_sql_rows *rows)
{
    (void)rows;
}

sc_status sc_sql_postgres_simple(sc_db *db, const char *text, sc_sql_error *error)
{
    (void)db, (void)text;
    return absent(error);
}

int sc_sql_postgres_is_null(const sc_sql_rows *rows, uint32_t column)
{
    (void)rows, (void)column;
    return 1;
}

const char *sc_sql_postgres_text(const sc_sql_rows *rows, uint32_t column, uint32_t *size)
{
    (void)rows, (void)column, (void)size;
    return NULL;
}

void sc_sql_postgres_forget(sc_db *db)
{
    (void)db;
}

#endif /* SC_DB_WITH_POSTGRESQL */
