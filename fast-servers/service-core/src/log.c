/*
 * One JSON object per line on stderr, in the shape contracts/logging.json defines.
 *
 * stderr and not stdout: the roles share the stream, and a log line must not end up mixed into
 * whatever a future subcommand writes for a human to read.
 */
#include "service_core/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <uv.h>

#if defined(_WIN32)
#include <sys/timeb.h>
#endif

/* Mirrors sc_log_cat. Kept adjacent so a category added to one and not the other is visible
 * in the same screenful. */
static const char *const kCategoryNames[SC_CAT__COUNT] = {
    "auth",       "user", "transaction", "contribution", "community",
    "federation", "http", "db",          "session",      "startup",
    "mail",
};

static sc_log_level g_minimum = SC_LOG_INFO;
/* Initialised on the first sc_log_init and never destroyed: the log outlives everything that
 * would be in a position to tear it down. */
static uv_mutex_t g_write_lock;
static int g_write_lock_ready;

int64_t sc_now_ms(void)
{
#if defined(_WIN32)
    struct _timeb tb;
    _ftime(&tb);
    return (int64_t)tb.time * 1000 + tb.millitm;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

void sc_log_init(sc_log_level minimum)
{
    g_minimum = minimum;
    if (!g_write_lock_ready && uv_mutex_init(&g_write_lock) == 0)
        g_write_lock_ready = 1;
}

sc_log_level sc_log_level_from_name(const char *name, sc_log_level fallback)
{
    if (name == NULL)
        return fallback;
    if (strcmp(name, "trace") == 0)
        return SC_LOG_TRACE;
    if (strcmp(name, "debug") == 0)
        return SC_LOG_DEBUG;
    if (strcmp(name, "info") == 0)
        return SC_LOG_INFO;
    if (strcmp(name, "warn") == 0 || strcmp(name, "warning") == 0)
        return SC_LOG_WARN;
    if (strcmp(name, "error") == 0)
        return SC_LOG_ERROR;
    if (strcmp(name, "fatal") == 0)
        return SC_LOG_FATAL;
    return fallback;
}

/*
 * Copies @p src into @p dst with the escaping JSON requires, stopping at @p dst_size - 1 bytes.
 * Returns the number of bytes written.
 *
 * The escape set is the minimum RFC 8259 asks for: quote, backslash and everything below 0x20.
 * Bytes above 0x7f are passed through -- the input is already UTF-8 and re-encoding it as \u
 * escapes would only make the line longer and harder to read.
 */
static size_t json_escape(char *dst, size_t dst_size, const char *src)
{
    static const char kHex[] = "0123456789abcdef";
    size_t out = 0;
    size_t i;

    if (dst_size == 0)
        return 0;
    for (i = 0; src[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)src[i];
        char escaped[6];
        size_t len;

        if (c == '"' || c == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)c;
            len = 2;
        } else if (c == '\n') {
            memcpy(escaped, "\\n", 2);
            len = 2;
        } else if (c == '\r') {
            memcpy(escaped, "\\r", 2);
            len = 2;
        } else if (c == '\t') {
            memcpy(escaped, "\\t", 2);
            len = 2;
        } else if (c < 0x20) {
            memcpy(escaped, "\\u00", 4);
            escaped[4] = kHex[c >> 4];
            escaped[5] = kHex[c & 0x0f];
            len = 6;
        } else {
            escaped[0] = (char)c;
            len = 1;
        }
        if (out + len >= dst_size)
            break;
        memcpy(dst + out, escaped, len);
        out += len;
    }
    dst[out] = '\0';
    return out;
}

/*
 * The optional envelope fields, rendered into @p dst as `,"key":value` runs.
 *
 * Returns what it wrote. A member that would not fit is left out and the object it is in is
 * closed all the same, so the line stays well-formed JSON: this is the same exception log.c
 * already makes for the sentence -- losing part of a diagnostic beats losing the event -- and
 * it never applies to the five required fields, which are written by the caller below.
 */
static size_t render_context(char *dst, size_t dst_size, const sc_log_context *context)
{
    char escaped[SC_LOG_LINE_MAX];
    size_t out = 0;
    size_t i;
    int n;

    if (context == NULL)
        return 0;

    if (context->usr != 0) {
        n = snprintf(dst + out, dst_size - out, ",\"usr\":%llu",
                     (unsigned long long)context->usr);
        if (n > 0 && (size_t)n < dst_size - out)
            out += (size_t)n;
    }
    if (context->err_name != NULL) {
        (void)json_escape(escaped, sizeof(escaped), context->err_name);
        n = snprintf(dst + out, dst_size - out, ",\"err\":{\"code\":%u,\"name\":\"%s\"}",
                     (unsigned)context->err_code, escaped);
        if (n > 0 && (size_t)n < dst_size - out)
            out += (size_t)n;
    }
    if (context->data == NULL || context->data_count == 0)
        return out;

    n = snprintf(dst + out, dst_size - out, ",\"data\":{");
    if (n <= 0 || (size_t)n >= dst_size - out)
        return out;
    out += (size_t)n;

    for (i = 0; i != context->data_count; ++i) {
        const sc_log_value *value = &context->data[i];
        const char *separator = (i == 0) ? "" : ",";

        (void)json_escape(escaped, sizeof(escaped), value->key);
        switch (value->kind) {
        case SC_LOG_VALUE_STRING: {
            char text[SC_LOG_LINE_MAX];
            (void)json_escape(text, sizeof(text), value->text != NULL ? value->text : "");
            n = snprintf(dst + out, dst_size - out, "%s\"%s\":\"%s\"", separator, escaped, text);
            break;
        }
        case SC_LOG_VALUE_INT:
            n = snprintf(dst + out, dst_size - out, "%s\"%s\":%lld", separator, escaped,
                         (long long)value->number);
            break;
        case SC_LOG_VALUE_UINT:
            n = snprintf(dst + out, dst_size - out, "%s\"%s\":%llu", separator, escaped,
                         (unsigned long long)value->unumber);
            break;
        case SC_LOG_VALUE_BOOL:
            n = snprintf(dst + out, dst_size - out, "%s\"%s\":%s", separator, escaped,
                         value->number != 0 ? "true" : "false");
            break;
        case SC_LOG_VALUE_NULL:
        default:
            n = snprintf(dst + out, dst_size - out, "%s\"%s\":null", separator, escaped);
            break;
        }
        if (n <= 0 || (size_t)n >= dst_size - out)
            break;
        out += (size_t)n;
    }

    /* Room for the closing brace is kept by the loop's own bound: it stops as soon as a member
     * would fill the buffer, and dst_size - out is then at least the terminator plus this. */
    if (out + 1 < dst_size) {
        dst[out++] = '}';
        dst[out] = '\0';
    }
    return out;
}

static void log_line(sc_log_level level, sc_log_cat cat, const char *event,
                     const sc_log_context *context, const char *fmt, va_list args)
{
    char message[SC_LOG_LINE_MAX];
    char escaped[SC_LOG_LINE_MAX];
    char extra[SC_LOG_LINE_MAX];
    char line[SC_LOG_LINE_MAX * 3];
    const char *category;
    int written;

    if (level < g_minimum)
        return;
    category = (cat >= 0 && cat < SC_CAT__COUNT) ? kCategoryNames[cat] : "startup";

    /* Truncation here is deliberate and is the one place this codebase permits it: losing the
     * tail of a sentence beats losing the event. The structure around it is never truncated. */
    (void)vsnprintf(message, sizeof(message), fmt, args);
    message[sizeof(message) - 1] = '\0';
    (void)json_escape(escaped, sizeof(escaped), message);

    extra[0] = '\0';
    (void)render_context(extra, sizeof(extra), context);

    /* msg last, so that a line read by a human ends with the sentence rather than with the
     * fields -- the order is not contracted, and nothing parses this by position. */
    written = snprintf(line, sizeof(line),
                       "{\"time\":%lld,\"level\":%d,\"cat\":\"%s\",\"event\":\"%s\"%s"
                       ",\"msg\":\"%s\"}\n",
                       (long long)sc_now_ms(), (int)level, category, event, extra, escaped);
    if (written <= 0)
        return;
    if ((size_t)written >= sizeof(line))
        written = (int)sizeof(line) - 1;

    /* One lock, one fwrite: two roles logging at the same moment must not interleave halves of
     * a line, because a half line is not JSON and the tests parse this stream. */
    if (g_write_lock_ready)
        uv_mutex_lock(&g_write_lock);
    (void)fwrite(line, 1, (size_t)written, stderr);
    (void)fflush(stderr);
    if (g_write_lock_ready)
        uv_mutex_unlock(&g_write_lock);
}

void sc_log(sc_log_level level, sc_log_cat cat, const char *event, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    log_line(level, cat, event, NULL, fmt, args);
    va_end(args);
}

void sc_log_event(sc_log_level level, sc_log_cat cat, const char *event,
                  const sc_log_context *context, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    log_line(level, cat, event, context, fmt, args);
    va_end(args);
}
