/*
 * The part of running a statement that does not depend on the database: giving a statement its
 * slot, dispatching to the driver, and reading a column whose text PostgreSQL has already
 * produced. service_core/sql.h holds the design.
 */
#include "service_core/sql.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db_internal.h"
#include "service_core/atomic.h"
#include "service_core/log/log.h"

/* The last slot handed out. Slots start at 1, so 0 can mean "not yet". */
static volatile int32_t g_last_slot = 0;

/*
 * The statement's slot, giving it one on first use.
 *
 * Two threads meeting a new statement at once both draw a number and only one of them is kept;
 * the other number is never used by anybody. That wastes a slot per race, which happens once
 * per statement per process at most -- cheaper than a lock on every statement ever run.
 */
static int32_t slot_of(sc_sql_statement *statement)
{
    int32_t slot = sc_atomic_load(&statement->slot);
    int32_t drawn;

    if (slot != 0)
        return slot;
    drawn = sc_atomic_inc(&g_last_slot);
    if (drawn >= SC_SQL_STATEMENTS_MAX) {
        sc_log_error(SC_CAT_DB, "db.query.failed",
                     "statement %s needs slot %d and this build has %d -- raise "
                     "SC_SQL_STATEMENTS_MAX",
                     statement->name, (int)drawn, (int)SC_SQL_STATEMENTS_MAX);
        return -1;
    }
    if (sc_atomic_cas(&statement->slot, 0, drawn))
        return drawn;
    return sc_atomic_load(&statement->slot);
}

void sc_sql_set_error(sc_sql_error *error, sc_sql_error_kind kind, const char *message)
{
    size_t out = 0;
    size_t i;

    if (error == NULL)
        return;
    error->kind = kind;
    if (message == NULL || message[0] == '\0')
        message = "the driver gave no reason";
    /* libpq ends a message with a newline and indents its hint onto the next line; a log line
     * is a line, so every run of whitespace becomes one space. */
    for (i = 0; out + 1 < sizeof(error->message) && message[i] != '\0'; ++i) {
        char c = message[i];

        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
        if (c == ' ' && (out == 0 || error->message[out - 1] == ' '))
            continue;
        error->message[out++] = c;
    }
    while (out > 0 && error->message[out - 1] == ' ')
        --out;
    error->message[out] = '\0';
}

static void clear_error(sc_sql_error *error)
{
    if (error == NULL)
        return;
    error->kind = SC_SQL_ERROR_NONE;
    error->constraint[0] = '\0';
    error->message[0] = '\0';
}

static sc_status run(sc_db *db, sc_sql_statement *statement, const sc_sql_param *params,
                     uint32_t param_count, sc_sql_rows *rows, int64_t *changes, sc_sql_error *error)
{
    int32_t slot;

    clear_error(error);
    if (changes != NULL)
        *changes = 0;
    if (db == NULL || statement == NULL || (param_count != 0 && params == NULL) ||
        param_count > SC_SQL_PARAMS_MAX) {
        sc_sql_set_error(error, SC_SQL_ERROR_OTHER, "invalid arguments to a statement");
        return SC_ERR_INVALID_ARGUMENT;
    }
    slot = slot_of(statement);
    if (slot < 0) {
        sc_sql_set_error(error, SC_SQL_ERROR_OTHER, "no statement slot left");
        return SC_ERR_TOO_LONG;
    }
    if (db->kind == SC_DB_SQLITE)
        return sc_sql_sqlite_run(db, statement, slot, params, param_count, rows, changes, error);
    return sc_sql_postgres_run(db, statement, slot, params, param_count, rows, changes, error);
}

sc_status sc_sql_query(sc_db *db, sc_sql_statement *statement, const sc_sql_param *params,
                       uint32_t param_count, sc_sql_rows *rows, sc_sql_error *error)
{
    if (rows == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    memset(rows, 0, sizeof(*rows));
    return run(db, statement, params, param_count, rows, NULL, error);
}

sc_status sc_sql_exec(sc_db *db, sc_sql_statement *statement, const sc_sql_param *params,
                      uint32_t param_count, int64_t *changes, sc_sql_error *error)
{
    return run(db, statement, params, param_count, NULL, changes, error);
}

sc_status sc_sql_simple(sc_db *db, const char *text, sc_sql_error *error)
{
    clear_error(error);
    if (db == NULL || text == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    if (db->kind == SC_DB_SQLITE)
        return sc_sql_sqlite_simple(db, text, error);
    return sc_sql_postgres_simple(db, text, error);
}

int sc_sql_next(sc_sql_rows *rows)
{
    if (rows == NULL || rows->db == NULL || rows->handle == NULL)
        return 0;
    if (rows->db->kind == SC_DB_SQLITE)
        return sc_sql_sqlite_next(rows);
    return sc_sql_postgres_next(rows);
}

void sc_sql_close(sc_sql_rows *rows)
{
    if (rows == NULL || rows->db == NULL || rows->handle == NULL)
        return;
    if (rows->db->kind == SC_DB_SQLITE)
        sc_sql_sqlite_close(rows);
    else
        sc_sql_postgres_close(rows);
    rows->handle = NULL;
}

/* --- columns ------------------------------------------------------------------------------
 *
 * SQLite hands every type over as itself. PostgreSQL hands everything over as text -- the
 * format is per statement rather than per column, and numeric, which the money columns will
 * be, has a binary form nobody wants to decode by hand -- so its readers parse. The session is
 * opened with TimeZone UTC and DateStyle ISO (db_postgres.c), which is what makes a timestamp's
 * text a single, parseable shape.
 */

int sc_sql_col_is_null(const sc_sql_rows *rows, uint32_t column)
{
    if (rows == NULL || rows->db == NULL || rows->handle == NULL)
        return 1;
    if (rows->db->kind == SC_DB_SQLITE)
        return sc_sql_sqlite_is_null(rows, column);
    return sc_sql_postgres_is_null(rows, column);
}

const char *sc_sql_col_text(const sc_sql_rows *rows, uint32_t column, uint32_t *size)
{
    const char *text;
    uint32_t length = 0;

    if (rows == NULL || rows->db == NULL || rows->handle == NULL) {
        text = NULL;
    } else if (rows->db->kind == SC_DB_SQLITE) {
        text = sc_sql_sqlite_text(rows, column, &length);
    } else {
        text = sc_sql_postgres_text(rows, column, &length);
    }
    if (size != NULL)
        *size = text != NULL ? length : 0;
    return text != NULL ? text : "";
}

int64_t sc_sql_col_int(const sc_sql_rows *rows, uint32_t column)
{
    const char *text;

    if (rows == NULL || rows->db == NULL || rows->handle == NULL)
        return 0;
    if (rows->db->kind == SC_DB_SQLITE)
        return sc_sql_sqlite_int(rows, column);
    text = sc_sql_postgres_text(rows, column, NULL);
    return text != NULL ? strtoll(text, NULL, 10) : 0;
}

int sc_sql_col_bool(const sc_sql_rows *rows, uint32_t column)
{
    const char *text;

    if (rows == NULL || rows->db == NULL || rows->handle == NULL)
        return 0;
    if (rows->db->kind == SC_DB_SQLITE)
        return sc_sql_sqlite_int(rows, column) != 0;
    text = sc_sql_postgres_text(rows, column, NULL);
    return text != NULL && text[0] == 't';
}

/* Days since 1970-01-01 of a proleptic Gregorian date -- Howard Hinnant's days_from_civil. No
 * timegm, which Windows spells differently and which consults a time zone database nobody
 * asked for. */
static int64_t days_from_civil(int64_t y, unsigned m, unsigned d)
{
    int64_t era;
    unsigned yoe;
    unsigned doy;
    unsigned doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* `2026-09-10 12:34:56.789+00` -- ISO, as the session is set up to answer. Fractions beyond
 * milliseconds are dropped; an offset other than +00 is honoured, though the session never
 * produces one. Answers 0 for text that is not a timestamp. */
static int64_t parse_timestamp(const char *text)
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int consumed = 0;
    int64_t millis = 0;
    int64_t offset_minutes = 0;
    const char *p;

    if (text == NULL || sscanf(text, "%d-%d-%d %d:%d:%d%n", &year, &month, &day, &hour, &minute,
                               &second, &consumed) != 6)
        return 0;
    p = text + consumed;
    if (*p == '.') {
        int digits = 0;

        for (++p; *p >= '0' && *p <= '9'; ++p, ++digits) {
            if (digits < 3)
                millis = millis * 10 + (*p - '0');
        }
        for (; digits < 3; ++digits)
            millis *= 10;
    }
    if (*p == '+' || *p == '-') {
        int sign = *p == '-' ? -1 : 1;
        int oh = 0;
        int om = 0;

        (void)sscanf(p + 1, "%2d:%2d", &oh, &om);
        offset_minutes = sign * (oh * 60 + om);
    }
    return ((days_from_civil(year, (unsigned)month, (unsigned)day) * 86400 + hour * 3600 +
             minute * 60 + second) -
            offset_minutes * 60) *
               1000 +
           millis;
}

int64_t sc_sql_col_time(const sc_sql_rows *rows, uint32_t column)
{
    if (rows == NULL || rows->db == NULL || rows->handle == NULL)
        return 0;
    if (rows->db->kind == SC_DB_SQLITE)
        return sc_sql_sqlite_int(rows, column);
    return parse_timestamp(sc_sql_postgres_text(rows, column, NULL));
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

int64_t sc_sql_col_bytes(const sc_sql_rows *rows, uint32_t column, uint8_t *out, size_t out_size)
{
    const char *text;
    uint32_t length = 0;
    uint32_t i;

    if (rows == NULL || rows->db == NULL || rows->handle == NULL || out == NULL)
        return -1;
    if (rows->db->kind == SC_DB_SQLITE)
        return sc_sql_sqlite_bytes(rows, column, out, out_size);
    /* bytea in text form is `\x` and two hex digits a byte. */
    text = sc_sql_postgres_text(rows, column, &length);
    if (text == NULL || length < 2 || text[0] != '\\' || text[1] != 'x' || (length & 1u) != 0 ||
        (length - 2) / 2 > out_size)
        return -1;
    for (i = 2; i != length; i += 2) {
        int high = hex_digit(text[i]);
        int low = hex_digit(text[i + 1]);

        if (high < 0 || low < 0)
            return -1;
        out[(i - 2) / 2] = (uint8_t)((high << 4) | low);
    }
    return (int64_t)((length - 2) / 2);
}

int sc_sql_col_copy(const sc_sql_rows *rows, uint32_t column, char *out, size_t out_size)
{
    uint32_t size = 0;
    const char *text = sc_sql_col_text(rows, column, &size);

    if (out == NULL || out_size == 0 || (size_t)size + 1 > out_size)
        return 0;
    memcpy(out, text, size);
    out[size] = '\0';
    return 1;
}
