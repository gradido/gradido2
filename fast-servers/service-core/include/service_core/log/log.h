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
 *
 * This header is what a caller that writes a line needs. Starting and stopping the logger is
 * `service_core/log/logger.h`, and only main and the tests include that one -- the same split
 * email/message.h and email/mailer.h make, and for the same reason: a caller that writes
 * should not have to know that a thread is doing the writing.
 *
 * **A line is written asynchronously.** sc_log() packs the record into an arena, hands it to a
 * ring and returns; one logger thread encodes and writes it. What that costs a caller, and
 * what it demands in return, is in logger.h -- the short version is that every value a line
 * carries is *copied* here rather than borrowed, because the caller's frame is gone by the
 * time the line is written. The exceptions are @ref sc_log_value::key, @p event and
 * @ref sc_log_context::err_name, which are string literals and outlive everything.
 */
#ifndef SERVICE_CORE_LOG_LOG_H
#define SERVICE_CORE_LOG_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/** Maps "info", "debug", ... onto a level, answering @p fallback for anything unrecognised. */
sc_log_level sc_log_level_from_name(const char *name, sc_log_level fallback);

/* --- the optional envelope fields ------------------------------------------------------- */

/*
 * contracts/logging.json, envelope: beyond time, level, cat, event and msg a line may carry
 * `req`, `usr`, `err` and `data`, and most contracted events do. What follows is how they are
 * handed over -- by value, in the caller's own frame, so that a caller still builds one on its
 * stack and nothing here allocates.
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

/**
 * One member of `data`. Written with the macros below, never field by field.
 *
 * @p key is a string literal and is taken as a pointer -- it lives in the program text and
 * outlives everything. @p text is copied when the line is handed over, because by the time the
 * logger thread reads it the caller's stack frame is gone.
 */
typedef struct sc_log_value {
    const char *key;
    sc_log_value_kind kind;
    /* Copied at submit time. NULL is written as an empty string rather than as JSON null --
     * SC_LOG_VALUE_NULL says null. Bytes that are not UTF-8 are cut where they stop being it and
     * the rest of the value becomes U+FFFD; see the note on sc_log() below. */
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
 * Longest sentence. Beyond it the sentence is truncated -- the one place in this codebase
 * where truncating beats failing, because the alternative is losing the event entirely. The
 * structure around it is never truncated.
 */
#define SC_LOG_MSG_MAX 1024

/**
 * What a line carries besides its sentence. Every field is optional and every field is absent
 * rather than null when it is not set -- the contract's third rule.
 */
typedef struct sc_log_context {
    /** `req`, the request correlation id. Copied. NULL leaves the field out. */
    const char *req;
    /** `usr`, users.id. 0 is absent: no row has it, the identity columns start at 1. */
    uint64_t usr;
    /** `err.name` from contracts/errors -- a literal, taken as a pointer. NULL leaves the
     *  whole `err` object out. */
    const char *err_name;
    /** `err.code`, read only when @ref err_name is set. */
    uint32_t err_code;
    /** `data`. Keys are literals and borrowed, string values are copied. */
    const sc_log_value *data;
    size_t data_count;
} sc_log_context;

/**
 * Writes one line. @p event is the stable dotted id tests assert on; @p fmt builds the human
 * sentence and is printf-shaped.
 *
 * The sentence is formatted on the caller's stack into SC_LOG_MSG_MAX bytes and the record is
 * handed to the logger thread. When the ring is full the call *waits* rather than dropping the
 * line -- see logger.h, which is where that decision is written down.
 *
 * **Bytes that are not UTF-8 cost the rest of the value, never the line.** @p fmt's result,
 * @ref sc_log_context::req and every string in `data` are cut where they stop being well formed
 * and finish with U+FFFD. That is not politeness: a JSON document holding malformed UTF-8 is not
 * JSON, and the encoder underneath refuses the whole document over one bad byte -- which would
 * lose the line silently, and lose it for exactly the input somebody opens the log to find. So
 * pass a request path or a header through as it came; what arrives is readable up to the first
 * bad byte and says so.
 */
void sc_log(sc_log_level level, sc_log_cat cat, const char *event, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;

/** sc_log with the optional envelope fields. @p context may be NULL, which is what sc_log is. */
void sc_log_event(sc_log_level level, sc_log_cat cat, const char *event,
                  const sc_log_context *context, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

/**
 * Around the ring, synchronous, never waits on it.
 *
 * For the health path and for anything that must not wait because someone else would read the
 * wait as death -- an orchestrator killing the instance in the middle of an incident is the
 * case this exists for. It costs one write(2) on the calling thread.
 */
void sc_log_direct(sc_log_level level, sc_log_cat cat, const char *event,
                   const sc_log_context *context, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 5, 6)))
#endif
    ;

/**
 * sc_log_direct at SC_LOG_FATAL, then abort(). Does not return.
 *
 * Around the ring for the same reason: without it the process dies with the line that explains
 * why still sitting in the logger's buffer.
 */
void sc_log_panic(sc_log_cat cat, const char *event, const sc_log_context *context,
                  const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5), noreturn))
#endif
    ;

#define sc_log_debug(cat, event, ...) sc_log(SC_LOG_DEBUG, (cat), (event), __VA_ARGS__)
#define sc_log_info(cat, event, ...) sc_log(SC_LOG_INFO, (cat), (event), __VA_ARGS__)
#define sc_log_warn(cat, event, ...) sc_log(SC_LOG_WARN, (cat), (event), __VA_ARGS__)
#define sc_log_error(cat, event, ...) sc_log(SC_LOG_ERROR, (cat), (event), __VA_ARGS__)
#define sc_log_fatal(cat, event, ...) sc_log(SC_LOG_FATAL, (cat), (event), __VA_ARGS__)

/** Unix milliseconds, UTC -- the `time` field of the envelope, exposed because callers that
 *  timestamp their own records (the dht drain, for one) must use the same clock. */
int64_t sc_now_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_CORE_LOG_LOG_H */
