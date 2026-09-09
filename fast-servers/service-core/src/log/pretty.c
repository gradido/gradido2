/*
 * The human line, in the style of pino-pretty.
 *
 * Two passes over the same record: one that measures it exactly and one that writes it. The
 * first exists because a pretty line has no bound -- see sc_log_pretty_measure() -- and it pays for
 * itself, because the second reads its lengths back rather than working them out again.
 */
#include "format.h"

#include <string.h>
#include <time.h>

#include "arnm/converter.h"

/*
 * libuv has no calendar call -- it deals in durations and in wall clock microseconds, not in
 * broken down time -- so this one stays with the C library.
 *
 * gmtime_r is POSIX and Windows has none of it, MinGW included; the CRT spells the same thing
 * gmtime_s, with the arguments the other way round from C11 Annex K's and an errno_t rather
 * than a pointer as the answer. The guard is _WIN32 and not _MSC_VER for that reason -- a
 * mingw cross build wants the CRT's spelling just as much as MSVC does.
 */
static int to_utc(struct tm *out, time_t secs)
{
#if defined(_WIN32)
    return gmtime_s(out, &secs) == 0;
#else
    return gmtime_r(&secs, out) != NULL;
#endif
}

static const char *level_name(uint8_t level)
{
    switch (level) {
    case SC_LOG_TRACE: return "TRACE";
    case SC_LOG_DEBUG: return "DEBUG";
    case SC_LOG_WARN:  return "WARN";
    case SC_LOG_ERROR: return "ERROR";
    case SC_LOG_FATAL: return "FATAL";
    default:          return "INFO";
    }
}

static const char *level_color(uint8_t level)
{
    switch (level) {
    case SC_LOG_TRACE: return "\033[90m";
    case SC_LOG_DEBUG: return "\033[36m";
    case SC_LOG_WARN:  return "\033[33m";
    case SC_LOG_ERROR: return "\033[31m";
    case SC_LOG_FATAL: return "\033[1;31m";
    default:          return "\033[32m";
    }
}

/* The frame of the line. Both passes take these lengths from the literal itself, so neither
 * can be wrong about one. */
#define LIT_LEN(s) ((uint32_t)(sizeof(s) - 1u))
#define PUT_LIT(b, s) unsafe_arnm_byte_buffer_copy((b), (s), LIT_LEN(s))
#define PUT_RUN(b, p, n) unsafe_arnm_byte_buffer_copy((b), (p), (n))
#define PUT_BYTE(b, c) unsafe_arnm_byte_buffer_push((b), (uint8_t)(c))

#define GREY "\033[90m"
#define RESET "\033[0m"

/* A clock field is fixed width, which the converter does not do -- so ask it how long the
 * number is and put the missing zeros in front of it. */
static char *b_pad(char *p, uint64_t v, uint8_t width)
{
    uint8_t n = arnm_uint64_to_string_size(v);
    uint8_t zeros = n < width ? (uint8_t)(width - n) : 0u;
    while (zeros-- > 0u)
        *p++ = '0';
    return p + arnm_uint64_to_string_known_string_size(p, v, n);
}

static void put_u64(arnm_byte_buffer *b, uint64_t v, uint8_t digits)
{
    char tmp[21]; /* UINT64_MAX and its terminator */
    arnm_uint64_to_string_known_string_size(tmp, v, digits);
    unsafe_arnm_byte_buffer_copy(b, tmp, digits);
}

static void put_i64(arnm_byte_buffer *b, int64_t v, uint8_t digits)
{
    char tmp[22]; /* the same, with room for the sign */
    arnm_int64_to_string_known_string_size(tmp, v, digits);
    unsafe_arnm_byte_buffer_copy(b, tmp, digits);
}

/*
 * The exact number of bytes sc_log_encode_pretty() will write for this record.
 *
 * Not a bound and not an estimate. No constant can bound a pretty line: `event`, `err_name`
 * and every `data[i].key` are pointers into the caller's rodata that submit() borrows rather
 * than copies, so the SC_LOG_GRADE_MAX cap on the arena never sees them -- and ten numbers held as
 * eight bytes apiece spell up to twenty digits each. A line therefore has no length until it
 * is measured, which is what this does, and what lets the write pass stop checking.
 *
 * It walks the same fields in the same order as sc_log_encode_pretty(), and nothing in the language
 * keeps the two in step. tests/test_pretty_len.c does, over every shape the record has.
 */
size_t sc_log_pretty_measure(sc_log_pretty_plan *p, const sc_log_record *r, int color)
{
    size_t n;
    uint16_t i;

    p->req = 0u;
    p->err_name = 0u;
    p->usr = 0u;
    p->err_code = 0u;
    p->level_color = color ? (uint32_t)strlen(level_color(r->level)) : 0u;
    p->level = (uint32_t)strlen(level_name(r->level));
    p->cat = (uint32_t)strlen(kScLogCatNames[r->cat < SC_CAT__COUNT ? r->cat : SC_CAT_STARTUP]);
    p->event = (uint32_t)strlen(r->event);
    p->msg = (uint32_t)strlen(r->msg);

    /* GREY [ HH:MM:SS.mmm ] RESET ' ' -- the stamp is twelve whatever the clock says: two
     * digits for the hour, two each for minute and second, three for the millisecond, and
     * three separators. b_pad() only ever adds zeros; it never adds a digit. */
    n = (color ? LIT_LEN(GREY) + LIT_LEN(RESET) : 0u) + 1u + 12u + 1u + 1u;
    n += p->level_color + p->level + (color ? LIT_LEN(RESET) : 0u);
    n += LIT_LEN(" (") + p->cat + 1u + p->event + LIT_LEN("): ") + p->msg;

    if (r->req != NULL || r->usr != 0 || r->err_name != NULL || r->data_count > 0) {
        if (color)
            n += LIT_LEN(GREY);
        if (r->req != NULL) {
            p->req = (uint32_t)strlen(r->req);
            n += LIT_LEN("  req=") + p->req;
        }
        if (r->usr != 0) {
            p->usr = arnm_uint64_to_string_size(r->usr);
            n += LIT_LEN("  usr=") + p->usr;
        }
        if (r->err_name != NULL) {
            p->err_name = (uint32_t)strlen(r->err_name);
            p->err_code = arnm_uint64_to_string_size(r->err_code);
            n += LIT_LEN("  err=") + p->err_name + 1u + p->err_code + 1u;
        }
        for (i = 0; i < r->data_count; ++i) {
            const sc_log_value *v = &r->data[i];
            p->key[i] = (uint32_t)strlen(v->key);
            switch (v->kind) {
            case SC_LOG_VALUE_STRING:
                p->val[i] = v->text != NULL ? (uint32_t)strlen(v->text) : 0u;
                break;
            case SC_LOG_VALUE_INT:
                p->val[i] = arnm_int64_to_string_size(v->number);
                break;
            case SC_LOG_VALUE_UINT:
                p->val[i] = arnm_uint64_to_string_size(v->unumber);
                break;
            case SC_LOG_VALUE_BOOL:
                p->val[i] = v->number != 0 ? LIT_LEN("true") : LIT_LEN("false");
                break;
            default:
                p->val[i] = LIT_LEN("null");
                break;
            }
            n += LIT_LEN("  ") + p->key[i] + 1u + p->val[i];
        }
        if (color)
            n += LIT_LEN(RESET);
    }
    p->total = n + 1u; /* the newline */
    return p->total;
}

/* In the style of pino-pretty: [12:34:56.789] INFO (http/request.complete): text  k=v k=v
 *
 * Every append here is unchecked. What makes that sound is the caller: it has asked
 * sc_log_pretty_measure() for the exact length and arnm_byte_buffer_available() for the room, and
 * writes nothing unless the second covers the first. Under a debug build the unsafe pair
 * asserts each precondition anyway, which is what tests/test_pretty_len.c leans on.
 *
 * A run of length 0 goes through rather than round: an empty message and an empty data value
 * are ordinary, arnm takes a size of 0 as the no-op it is, and guarding some twenty appends
 * against it cost more than the copies it saved. The pointer still has to be one -- hence the
 * empty string where a data value carries none. */
void sc_log_encode_pretty(arnm_byte_buffer *b, const sc_log_pretty_plan *p, const sc_log_record *r,
                          int color)
{
    time_t secs = (time_t)(r->time_ms / 1000);
    long ms = (long)(r->time_ms % 1000);
    struct tm tmv;
    uint16_t i;
    char stamp[16]; /* twelve characters, and the converter's terminator as the thirteenth --
                     * it lands past the last digit and is never written out */

    if (to_utc(&tmv, secs)) {
        char *q = stamp;
        q = b_pad(q, (uint64_t)tmv.tm_hour, 2u);
        *q++ = ':';
        q = b_pad(q, (uint64_t)tmv.tm_min, 2u);
        *q++ = ':';
        q = b_pad(q, (uint64_t)tmv.tm_sec, 2u);
        *q++ = '.';
        (void)b_pad(q, (uint64_t)ms, 3u);
    } else {
        memcpy(stamp, "??:??:??.???", 12);
    }

    if (color) PUT_LIT(b, GREY);
    PUT_BYTE(b, '[');
    unsafe_arnm_byte_buffer_copy(b, stamp, 12u);
    PUT_BYTE(b, ']');
    if (color) PUT_LIT(b, RESET);
    PUT_BYTE(b, ' ');

    if (color) PUT_RUN(b, level_color(r->level), p->level_color);
    PUT_RUN(b, level_name(r->level), p->level);
    if (color) PUT_LIT(b, RESET);

    PUT_LIT(b, " (");
    PUT_RUN(b, kScLogCatNames[r->cat < SC_CAT__COUNT ? r->cat : SC_CAT_STARTUP], p->cat);
    PUT_BYTE(b, '/');
    PUT_RUN(b, r->event, p->event);
    PUT_LIT(b, "): ");
    PUT_RUN(b, r->msg, p->msg);

    if (r->req != NULL || r->usr != 0 || r->err_name != NULL || r->data_count > 0) {
        if (color) PUT_LIT(b, GREY);
        if (r->req != NULL) {
            PUT_LIT(b, "  req=");
            PUT_RUN(b, r->req, p->req);
        }
        if (r->usr != 0) {
            PUT_LIT(b, "  usr=");
            put_u64(b, r->usr, p->usr);
        }
        if (r->err_name != NULL) {
            PUT_LIT(b, "  err=");
            PUT_RUN(b, r->err_name, p->err_name);
            PUT_BYTE(b, '(');
            put_u64(b, r->err_code, p->err_code);
            PUT_BYTE(b, ')');
        }
        for (i = 0; i < r->data_count; ++i) {
            const sc_log_value *v = &r->data[i];
            PUT_LIT(b, "  ");
            PUT_RUN(b, v->key, p->key[i]);
            PUT_BYTE(b, '=');
            switch (v->kind) {
            case SC_LOG_VALUE_STRING:
                PUT_RUN(b, v->text != NULL ? v->text : "", p->val[i]);
                break;
            case SC_LOG_VALUE_INT:
                put_i64(b, v->number, (uint8_t)p->val[i]);
                break;
            case SC_LOG_VALUE_UINT:
                put_u64(b, v->unumber, (uint8_t)p->val[i]);
                break;
            case SC_LOG_VALUE_BOOL:
                if (v->number != 0) PUT_LIT(b, "true"); else PUT_LIT(b, "false");
                break;
            default:
                PUT_LIT(b, "null");
                break;
            }
        }
        if (color) PUT_LIT(b, RESET);
    }
    PUT_BYTE(b, '\n');
}
