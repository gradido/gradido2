/*
 * Statements: how every repository talks to either database.
 *
 * ### Why this exists, when db.h used to say it must not
 *
 * db.h once held that there is "no query surface both drivers implement", so that a repository
 * has to say which dialect it is writing. That reason still holds, and this keeps it: a statement
 * carries its PostgreSQL text *and* its SQLite text, side by side, written by the repository and
 * reviewed as a pair. What is shared is everything that is not dialect -- binding a value,
 * stepping a row, reading a column, telling a unique violation from a dead connection.
 *
 * What changed is what that sharing buys. A repository that calls PQexecParams itself decides,
 * at every call site, that the statement is parsed and planned again on every call, that the
 * call blocks the thread it is on, and that its failure is read out of a driver's own error
 * fields. Hundreds of such call sites make each of those decisions permanent. Behind this
 * surface they are one file's: the statement is prepared once per connection, where it runs is
 * the executor's business (service_core/db_exec.h), and a unique violation is the same value on
 * both databases. `Architecture.md`, *Databases*, has the reasoning.
 *
 * ### A statement is an object, not a string
 *
 *   static sc_sql_statement kInsertUser = SC_SQL_STATEMENT("user.insert",
 *       "INSERT INTO users (...) VALUES ($1, $2) RETURNING id",
 *       "INSERT INTO users (...) VALUES (?1, ?2) RETURNING id");
 *
 * File scope and not const: the first use gives it a slot, the same slot on every connection,
 * and each connection keeps its prepared form there -- PQprepare'd on PostgreSQL, a persistent
 * sqlite3_stmt on SQLite. Nothing is registered up front; a statement exists from the moment it
 * is first run. The name is for the log and for pg_stat_statements, not for lookups.
 *
 * ### Values
 *
 * Six types, and each has one meaning on both databases however differently they store it:
 *
 *   INT     int64                          bigint / INTEGER
 *   TEXT    UTF-8 bytes                    varchar, text, uuid / TEXT
 *   BOOL    0 or 1                         boolean / INTEGER 0 or 1
 *   TIME    Unix milliseconds, UTC         timestamptz(3) / INTEGER milliseconds
 *   BYTES   raw bytes                      bytea / BLOB
 *   NULL
 *
 * Parameters are borrowed: they must stay where they are until sc_sql_close (or until
 * sc_sql_exec returns). Column values are borrowed from the cursor and valid until the next
 * sc_sql_next or sc_sql_close.
 *
 * ### Threads
 *
 * A connection is used by one thread at a time, and so is everything here that takes one --
 * which the executor guarantees by giving every connection an owner. Nothing in this file locks.
 */
#ifndef SERVICE_CORE_SQL_H
#define SERVICE_CORE_SQL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "service_core/db.h"
#include "service_core/status.h"

/** Parameters one statement may take. */
#define SC_SQL_PARAMS_MAX 32
/**
 * Distinct statements one process may run. A slot per statement per connection, so this is also
 * the size of each connection's table of prepared statements -- a pointer each, 4 KiB.
 */
#define SC_SQL_STATEMENTS_MAX 512
/** A constraint name as PostgreSQL gives it, or SQLite's column list. */
#define SC_SQL_CONSTRAINT_MAX 128
/** What either driver says about a refused statement. */
#define SC_SQL_MESSAGE_MAX 512

typedef struct sc_sql_statement {
    const char *name;
    const char *postgresql;
    const char *sqlite;
    /* 0 until first use, then the slot every connection keeps this statement in. Written once,
     * with a compare-and-swap, by whichever thread gets there first. */
    volatile int32_t slot;
} sc_sql_statement;

#define SC_SQL_STATEMENT(name_, postgresql_, sqlite_) {(name_), (postgresql_), (sqlite_), 0}

typedef enum sc_sql_type {
    SC_SQL_NULL = 0,
    SC_SQL_INT,
    SC_SQL_TEXT,
    SC_SQL_BOOL,
    SC_SQL_TIME,
    SC_SQL_BYTES
} sc_sql_type;

typedef struct sc_sql_param {
    sc_sql_type type;
    uint32_t size;    /* TEXT and BYTES */
    int64_t number;   /* INT, BOOL, TIME */
    const void *data; /* TEXT and BYTES */
    /* TEXT only: data is NUL-terminated at size, which PostgreSQL's text format needs. A text
     * that is not is copied into a terminated buffer on the way -- see sc_sql_textn. */
    int terminated;
} sc_sql_param;

static inline sc_sql_param sc_sql_null(void)
{
    sc_sql_param p;

    memset(&p, 0, sizeof(p));
    p.type = SC_SQL_NULL;
    return p;
}

static inline sc_sql_param sc_sql_int(int64_t value)
{
    sc_sql_param p = sc_sql_null();

    p.type = SC_SQL_INT;
    p.number = value;
    return p;
}

static inline sc_sql_param sc_sql_bool(int value)
{
    sc_sql_param p = sc_sql_null();

    p.type = SC_SQL_BOOL;
    p.number = value != 0;
    return p;
}

/** @p unix_ms is milliseconds since 1970, UTC -- what sc_now_ms() answers. */
static inline sc_sql_param sc_sql_time(int64_t unix_ms)
{
    sc_sql_param p = sc_sql_null();

    p.type = SC_SQL_TIME;
    p.number = unix_ms;
    return p;
}

/** A C string. NULL becomes SQL NULL, which is what a nullable column wants from one. */
static inline sc_sql_param sc_sql_text(const char *text)
{
    sc_sql_param p = sc_sql_null();

    if (text == NULL)
        return p;
    p.type = SC_SQL_TEXT;
    p.data = text;
    p.size = (uint32_t)strlen(text);
    p.terminated = 1;
    return p;
}

/** @p size bytes that need not be terminated -- a slice of a request body. Copied once on
 *  PostgreSQL, bound as they are on SQLite. */
static inline sc_sql_param sc_sql_textn(const char *text, uint32_t size)
{
    sc_sql_param p = sc_sql_null();

    p.type = SC_SQL_TEXT;
    p.data = text;
    p.size = size;
    return p;
}

static inline sc_sql_param sc_sql_bytes(const void *data, uint32_t size)
{
    sc_sql_param p = sc_sql_null();

    p.type = SC_SQL_BYTES;
    p.data = data;
    p.size = size;
    return p;
}

/** What kind of refusal a failed statement was. */
typedef enum sc_sql_error_kind {
    SC_SQL_ERROR_NONE = 0,
    /** A unique index refused the row; `constraint` says which. */
    SC_SQL_ERROR_UNIQUE,
    SC_SQL_ERROR_FOREIGN_KEY,
    SC_SQL_ERROR_NOT_NULL,
    /** The connection is gone. The executor redials it; the statement is not retried. */
    SC_SQL_ERROR_CONNECTION,
    SC_SQL_ERROR_OTHER
} sc_sql_error_kind;

/**
 * Why a statement failed, the same way on both databases.
 *
 * The one thing a caller may decide on is `kind`, and `constraint` when it is UNIQUE:
 * PostgreSQL names the constraint (`users_uuid_key`), SQLite names the columns
 * (`users.gradido_id, users.community_id`), and a repository that needs to tell two unique
 * indexes apart matches a column name that appears in both spellings. `message` is the
 * driver's own sentence, on one line, for the log.
 */
typedef struct sc_sql_error {
    sc_sql_error_kind kind;
    char constraint[SC_SQL_CONSTRAINT_MAX];
    char message[SC_SQL_MESSAGE_MAX];
} sc_sql_error;

/** A statement's rows, one at a time. On the caller's stack; nothing here allocates. */
typedef struct sc_sql_rows {
    sc_db *db;
    void *handle;    /* PGresult * or sqlite3_stmt * */
    int32_t row;     /* PostgreSQL: the current row, -1 before the first */
    int32_t count;   /* PostgreSQL: rows in the result */
    int32_t pending; /* SQLite: what the last step answered, not yet handed out */
} sc_sql_rows;

/**
 * Runs @p statement and positions a cursor before its first row.
 *
 * Answers SC_OK, or SC_ERR_NETWORK when the connection is gone and SC_ERR_INVALID_ARGUMENT for
 * every other refusal, with @p error saying which. On failure @p rows needs no sc_sql_close.
 *
 * The statement has been executed by the time this returns -- a constraint an INSERT ...
 * RETURNING violates is reported here, not on the first sc_sql_next.
 */
sc_status sc_sql_query(sc_db *db, sc_sql_statement *statement, const sc_sql_param *params,
                       uint32_t param_count, sc_sql_rows *rows, sc_sql_error *error);

/** Moves to the next row. 1 when there is one, 0 when the rows are done. */
int sc_sql_next(sc_sql_rows *rows);

/** Gives the cursor back. Safe on a cursor that is already closed. */
void sc_sql_close(sc_sql_rows *rows);

/**
 * Runs a statement that returns no rows. @p changes, when not NULL, receives how many rows it
 * inserted, updated or deleted.
 */
sc_status sc_sql_exec(sc_db *db, sc_sql_statement *statement, const sc_sql_param *params,
                      uint32_t param_count, int64_t *changes, sc_sql_error *error);

/**
 * Runs text with no parameters and no rows: DDL out of a migration, `BEGIN`, `COMMIT`. Not
 * prepared, because each of these runs once or is too cheap to plan.
 */
sc_status sc_sql_simple(sc_db *db, const char *text, sc_sql_error *error);

/* --- the current row --------------------------------------------------------------------- */

int sc_sql_col_is_null(const sc_sql_rows *rows, uint32_t column);
/** 0 for NULL, like every reader here; ask sc_sql_col_is_null where the difference matters. */
int64_t sc_sql_col_int(const sc_sql_rows *rows, uint32_t column);
int sc_sql_col_bool(const sc_sql_rows *rows, uint32_t column);
/** Unix milliseconds, UTC. */
int64_t sc_sql_col_time(const sc_sql_rows *rows, uint32_t column);
/** The bytes of a text column and their count; "" and 0 for NULL. Not a copy. */
const char *sc_sql_col_text(const sc_sql_rows *rows, uint32_t column, uint32_t *size);
/**
 * Copies a bytea / BLOB column into @p out. Answers the number of bytes, or -1 when the value
 * does not fit @p out_size or is not bytes at all.
 */
int64_t sc_sql_col_bytes(const sc_sql_rows *rows, uint32_t column, uint8_t *out, size_t out_size);

/** Copies a text column into @p out as a C string. 0 when it does not fit, which a caller
 *  treats as a row that does not match its contract rather than as a value to cut. */
int sc_sql_col_copy(const sc_sql_rows *rows, uint32_t column, char *out, size_t out_size);

#endif /* SERVICE_CORE_SQL_H */
