/*
 * The log line, as contracts/logging.json defines it.
 *
 * One JSON object per line on stderr:
 *
 *   {"time":1756000000000,"level":30,"cat":"startup","event":"server.listen","msg":"..."}
 *
 * The envelope field names and the numeric levels are the contract -- Pino writes the same
 * shape on the TypeScript path, and the tests compare the structure. `msg` is never compared
 * between implementations, so it is a sentence for a human and nothing depends on its wording.
 *
 * The category set is closed. Adding one is a change to contracts/logging.json, not to this
 * header; the enum below only mirrors what is already contracted.
 */
#ifndef SERVICE_CORE_LOG_H
#define SERVICE_CORE_LOG_H

#include <stddef.h>
#include <stdint.h>

/* Pino's numeric levels. The number is the contract, the spelling is not. */
typedef enum sc_log_level {
    SC_LOG_TRACE = 10,
    SC_LOG_DEBUG = 20,
    SC_LOG_INFO = 30,
    SC_LOG_WARN = 40,
    SC_LOG_ERROR = 50,
    SC_LOG_FATAL = 60
} sc_log_level;

/* contracts/logging.json, categories. A category is a place in the system, never a place in
 * the source tree -- that is what keeps it from growing one entry per class. */
typedef enum sc_log_cat {
    SC_CAT_AUTH = 0,
    SC_CAT_USER,
    SC_CAT_TRANSACTION,
    SC_CAT_CONTRIBUTION,
    SC_CAT_COMMUNITY,
    SC_CAT_FEDERATION,
    SC_CAT_HTTP,
    SC_CAT_DB,
    SC_CAT_SESSION,
    SC_CAT_STARTUP,
    SC_CAT__COUNT
} sc_log_cat;

/**
 * Sets the minimum level and prepares the writer lock. Call once, before any thread starts;
 * every role logs into the same stream and the lock is what keeps two lines from interleaving.
 */
void sc_log_init(sc_log_level minimum);

/** Maps "info", "debug", ... onto a level, answering @p fallback for anything unrecognised. */
sc_log_level sc_log_level_from_name(const char *name, sc_log_level fallback);

/**
 * Writes one line. @p event is the stable dotted id tests assert on; @p fmt builds the human
 * sentence and is printf-shaped.
 *
 * The line is assembled in a stack buffer of SC_LOG_LINE_MAX and written with a single fwrite.
 * A line that would not fit is truncated -- a log line is the one place in this codebase where
 * truncating beats failing, because the alternative is losing the event entirely.
 */
void sc_log(sc_log_level level, sc_log_cat cat, const char *event, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

#define SC_LOG_LINE_MAX 1024

#define sc_log_debug(cat, event, ...) sc_log(SC_LOG_DEBUG, (cat), (event), __VA_ARGS__)
#define sc_log_info(cat, event, ...) sc_log(SC_LOG_INFO, (cat), (event), __VA_ARGS__)
#define sc_log_warn(cat, event, ...) sc_log(SC_LOG_WARN, (cat), (event), __VA_ARGS__)
#define sc_log_error(cat, event, ...) sc_log(SC_LOG_ERROR, (cat), (event), __VA_ARGS__)
#define sc_log_fatal(cat, event, ...) sc_log(SC_LOG_FATAL, (cat), (event), __VA_ARGS__)

/** Unix milliseconds, UTC -- the `time` field of the envelope, exposed because callers that
 *  timestamp their own records (the dht drain, for one) must use the same clock. */
int64_t sc_now_ms(void);

#endif /* SERVICE_CORE_LOG_H */
