#include "backend_core/database/sql.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(SC_DB_WITH_SQLITE)
#include <sqlite3.h>
#endif
#if defined(SC_DB_WITH_POSTGRESQL)
#include <libpq-fe.h>
#endif

void bc_sql_set_error(char *error, size_t error_size, const char *message)
{
    size_t i = 0;

    if (error == NULL || error_size == 0)
        return;
    if (message == NULL || message[0] == '\0') {
        (void)snprintf(error, error_size, "the driver gave no message");
        return;
    }
    for (; i + 1 < error_size && message[i] != '\0'; ++i)
        error[i] = (message[i] == '\n' || message[i] == '\r') ? ' ' : message[i];
    while (i > 0 && error[i - 1] == ' ')
        --i;
    error[i] = '\0';
}

sc_status bc_sql_exec(sc_db *db, const char *sql, char *error, size_t error_size)
{
    if (db == NULL || sql == NULL || error == NULL || error_size == 0)
        return SC_ERR_INVALID_ARGUMENT;
    error[0] = '\0';

    switch (sc_db_kind_of(db)) {
    case SC_DB_SQLITE: {
#if defined(SC_DB_WITH_SQLITE)
        sqlite3 *handle = (sqlite3 *)sc_db_native(db);
        char *message = NULL;

        if (sqlite3_exec(handle, sql, NULL, NULL, &message) != SQLITE_OK) {
            bc_sql_set_error(error, error_size, message != NULL ? message : sqlite3_errmsg(handle));
            sqlite3_free(message);
            return SC_ERR_INVALID_ARGUMENT;
        }
        sqlite3_free(message);
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
        PGresult *result = PQexec(handle, sql);
        ExecStatusType status = result != NULL ? PQresultStatus(result) : PGRES_FATAL_ERROR;

        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
            bc_sql_set_error(error, error_size,
                             result != NULL ? PQresultErrorMessage(result)
                                            : PQerrorMessage(handle));
            PQclear(result);
            return SC_ERR_INVALID_ARGUMENT;
        }
        PQclear(result);
        return SC_OK;
#else
        bc_sql_set_error(error, error_size, "this build has no PostgreSQL driver");
        return SC_ERR_UNAVAILABLE;
#endif
    }
    }
}

void bc_sql_timestamp_text(int64_t unix_ms, char *out, size_t out_size)
{
    time_t seconds = (time_t)(unix_ms / 1000);
    int millis = (int)(unix_ms % 1000);
    struct tm utc;

    if (out == NULL || out_size == 0)
        return;
    /* Negative milliseconds would be a pre-1970 instant, which nothing here writes; the guard is
     * so that a clock that answers nonsense produces a refused statement rather than a wrong
     * timestamp. */
    if (millis < 0) {
        millis += 1000;
        seconds -= 1;
    }
#if defined(_WIN32)
    if (gmtime_s(&utc, &seconds) != 0) {
        out[0] = '\0';
        return;
    }
#else
    if (gmtime_r(&seconds, &utc) == NULL) {
        out[0] = '\0';
        return;
    }
#endif
    (void)snprintf(out, out_size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", utc.tm_year + 1900,
                   utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
}

int bc_sql_bytea_text(const uint8_t *bytes, size_t length, char *out, size_t out_size)
{
    static const char kHex[] = "0123456789abcdef";
    size_t i;

    if (bytes == NULL || out == NULL || out_size < length * 2 + 3)
        return 0;
    out[0] = '\\';
    out[1] = 'x';
    for (i = 0; i != length; ++i) {
        out[2 + i * 2] = kHex[bytes[i] >> 4];
        out[3 + i * 2] = kHex[bytes[i] & 0x0f];
    }
    out[2 + length * 2] = '\0';
    return 1;
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

size_t bc_sql_bytea_parse(const char *text, uint8_t *out, size_t out_size)
{
    size_t digits;
    size_t i;

    if (text == NULL || out == NULL || text[0] != '\\' || text[1] != 'x')
        return 0;
    digits = strlen(text + 2);
    if ((digits & 1u) != 0 || digits / 2 > out_size)
        return 0;
    for (i = 0; i != digits; i += 2) {
        int high = hex_digit(text[2 + i]);
        int low = hex_digit(text[3 + i]);

        if (high < 0 || low < 0)
            return 0;
        out[i / 2] = (uint8_t)((high << 4) | low);
    }
    return digits / 2;
}
