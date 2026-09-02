/*
 * Running a statement that has no parameters and returns no rows, and saying what went wrong.
 *
 * That is the whole of what the two dialects genuinely share, and it is why this exists where
 * `service_core/db.h` deliberately has no query surface: DDL and transaction control are the
 * same statement text on both databases, so writing `BEGIN` twice would be a copy rather than a
 * decision. Everything else -- every statement with a parameter or a result -- stays in the
 * repository, in a branch that says which dialect it is in. Architecture.md, *Databases*, is the
 * rule; this is the one exception to it, and it is not allowed to grow a second query function.
 *
 * The migrations use it because a migration is exactly this shape: text from the contract, no
 * parameters, no rows.
 */
#ifndef BACKEND_CORE_SQL_H
#define BACKEND_CORE_SQL_H

#include <stddef.h>
#include <stdint.h>

#include "service_core/db.h"
#include "service_core/status.h"

/** Long enough for what either driver says about a refused statement. */
#define BC_SQL_ERROR_MAX 512

/**
 * Runs @p sql on @p db. @p error receives the driver's own sentence on failure and is left an
 * empty string on success; it may not be NULL, because a failure nobody can describe is a
 * failure nobody can fix.
 *
 * Answers SC_ERR_UNAVAILABLE when this build has no driver for the database @p db is, and
 * SC_ERR_INVALID_ARGUMENT when the database refused the statement.
 */
sc_status bc_sql_exec(sc_db *db, const char *sql, char *error, size_t error_size);

/**
 * Copies @p message into @p error as one line, without a trailing newline -- libpq ends every
 * message with one and a log line is a line. NULL or an empty message leaves a sentence saying
 * the driver gave none, so a caller printing this never prints nothing at all.
 *
 * Truncating rather than refusing, because this is a diagnostic: the house rule that a
 * truncated value is worse than a refused one is about values that connect somewhere or deliver
 * to someone.
 */
void bc_sql_set_error(char *error, size_t error_size, const char *message);

/* --- PostgreSQL's text formats ------------------------------------------------------------ */

/*
 * Parameters go to PostgreSQL as text and results come back as text, uniformly: a mixed format
 * is per statement rather than per column, so a query selecting a name and a key would have to
 * pick one for both. Text costs a hex encoding on the key and nothing measurable on a path that
 * runs at startup; what it buys is one shape for every statement in this component.
 *
 * SQLite needs none of this -- its driver takes the C types as they are.
 */

/** `2026-09-02T13:45:12.345Z` plus the terminator. What timestamptz(3) is written with. */
#define BC_TIMESTAMP_TEXT_MAX 32

/** Renders @p unix_ms as the ISO 8601 instant PostgreSQL parses into a timestamptz. UTC, with
 *  milliseconds, because the column has precision 3 and the TypeScript path writes the same. */
void bc_sql_timestamp_text(int64_t unix_ms, char *out, size_t out_size);

/** Bytes to `\x` followed by lowercase hex -- PostgreSQL's own input and output form for bytea.
 *  Answers 0 when @p out_size is too small; a buffer of 2 * @p length + 3 always fits. */
int bc_sql_bytea_text(const uint8_t *bytes, size_t length, char *out, size_t out_size);

/** The inverse, for a bytea read back as text. Answers the number of bytes written, or 0 for a
 *  value that is not `\x` and an even number of hex digits, or one that would not fit. */
size_t bc_sql_bytea_parse(const char *text, uint8_t *out, size_t out_size);

#endif /* BACKEND_CORE_SQL_H */
