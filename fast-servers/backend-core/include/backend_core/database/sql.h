/*
 * Running text that has no parameters and returns no rows, and saying what went wrong in one
 * line.
 *
 * What is left here after service_core/sql.h took over every statement with a parameter or a
 * row: DDL out of a migration and the transaction control a migration wraps around it, both of
 * which are the same text on both databases and run once. Repositories do not use this -- a
 * repository runs sc_sql_statement objects and leaves transactions to the executor.
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

#endif /* BACKEND_CORE_SQL_H */
