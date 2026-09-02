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
    SC_CAT_MAIL,
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

/* --- the optional envelope fields ------------------------------------------------------- */

/*
 * contracts/logging.json, envelope: beyond time, level, cat, event and msg a line may carry
 * `usr`, `err` and `data`, and most contracted events do. What follows is how they are handed
 * over -- by value, in the caller's own frame, so that a line still costs no allocation.
 *
 * `data` is flat by contract ("no nesting beyond one level"), which is what lets one array of
 * key/value pairs describe every event there is.
 */

typedef enum sc_log_value_kind {
    SC_LOG_VALUE_STRING = 0,
    SC_LOG_VALUE_INT,
    SC_LOG_VALUE_UINT,
    SC_LOG_VALUE_BOOL,
    /* An explicitly null member. The contract has one: db.migration.denied's `expected`, which
     * is null when the database is simply ahead of this build. */
    SC_LOG_VALUE_NULL
} sc_log_value_kind;

/** One member of `data`. Written with the macros below, never field by field. */
typedef struct sc_log_value {
    const char *key;
    sc_log_value_kind kind;
    /* Borrowed for the duration of the call, like everything else on this path. NULL is
     * written as an empty string rather than as JSON null -- SC_LOG_VALUE_NULL says null. */
    const char *text;
    int64_t number;
    uint64_t unumber;
} sc_log_value;

#define SC_LOG_STR(key, value) {(key), SC_LOG_VALUE_STRING, (value), 0, 0}
#define SC_LOG_INT(key, value) {(key), SC_LOG_VALUE_INT, NULL, (int64_t)(value), 0}
#define SC_LOG_UINT(key, value) {(key), SC_LOG_VALUE_UINT, NULL, 0, (uint64_t)(value)}
#define SC_LOG_BOOL(key, value) {(key), SC_LOG_VALUE_BOOL, NULL, (value) ? 1 : 0, 0}
#define SC_LOG_NULL(key) {(key), SC_LOG_VALUE_NULL, NULL, 0, 0}

/** Members one `data` object may carry. The widest contracted event has four. */
#define SC_LOG_DATA_MAX 8

/**
 * What a line carries besides its sentence. Every field is optional and every field is absent
 * rather than null when it is not set -- the contract's third rule.
 */
typedef struct sc_log_context {
    /** `usr`, users.id. 0 is absent: no row has it, the identity columns start at 1. */
    uint64_t usr;
    /** `err.name` from contracts/errors. NULL leaves the whole `err` object out. */
    const char *err_name;
    /** `err.code`, read only when @ref err_name is set. */
    uint32_t err_code;
    /** `data`, borrowed. NULL or a count of 0 leaves the object out. */
    const sc_log_value *data;
    size_t data_count;
} sc_log_context;

/**
 * sc_log with the optional envelope fields. @p context may be NULL, which is what sc_log is.
 *
 * Truncation is the same rule and applies to the sentence alone: the structure around it --
 * the envelope, `data`, `err` -- is written whole or the line is not written, because a
 * half-written object is not JSON and the tests parse this stream.
 */
void sc_log_event(sc_log_level level, sc_log_cat cat, const char *event,
                  const sc_log_context *context, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
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
