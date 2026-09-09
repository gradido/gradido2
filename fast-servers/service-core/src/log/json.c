/*
 * The contracted line, through arnm/json_writer.h.
 *
 * Every string here is borrowed and not copied, which is what the writer wants and what this
 * logger can promise: submit() packed msg, req and the string values into the record's own
 * arena block, and event, err_name and the keys are literals. All of it outlives the write.
 */
#include "format.h"

#include <string.h>

#include "arnm/arena.h"
#include "arnm/json_writer.h"
#include "arnm/memory.h"
#include "arnm/memory_block.h"
#include "arnm/utf8.h"

/*
 * Scratch for one line: the document the writer builds and the text it lays down, both out of
 * a borrowed arena and neither of them from the host.
 *
 * Eight times the largest record, because submit() has already capped one at SC_LOG_GRADE_MAX
 * and the worst an escape can do to a byte is turn it into six (`\u00xx`, for a control
 * character that is not one of the three with a short form). Thread local rather than static:
 * the drain loop is one thread, but sc_log_direct() runs on whichever thread could not wait.
 */
#define JSON_SCRATCH_BYTES (SC_LOG_GRADE_MAX * 8u)
static _Thread_local uint8_t t_json_scratch[JSON_SCRATCH_BYTES];

/** Key, its length and "needs no escaping" in one argument -- every key here is a literal. */
#define K(literal) ARNM_JSON_WRITER_KEY(literal)

/*
 * A value out of the source, and one out of the world.
 *
 * The writer escapes nothing unless it is told to, so the two are kept apart here rather than
 * escaping everything on the chance that it matters. `cat`, `event` and `err_name` are literals
 * this program owns; `msg`, `req` and the string values came through a caller and can hold a
 * quote, a backslash or a control character.
 */
#define ADD_LITERAL(w, key, value)                                                                 \
    arnm_json_writer_add_string((w), key, (value), strlen(value))
#define ADD_TEXT(w, scratch, key, value, length) add_text((w), (scratch), key, (value), (length))

/** U+FFFD, in the bytes it is written as. What a value loses its tail to. */
#define REPLACEMENT_CHARACTER "\xef\xbf\xbd"

/*
 * A string that came through a caller, escaped -- and cut where it stops being UTF-8.
 *
 * The cut is not tidiness, it is the difference between a line and no line. Asking the writer to
 * escape a string is also what puts it under yyjson's UTF-8 check (arnm 0.8.1, json_writer.h,
 * *The build checks UTF-8*), and a string that fails that check fails the **whole document** at
 * arnm_json_writer_write() with ARNM_ERROR_ENCODE_FAILED. encode_json() then answers an error,
 * the logger writes nothing, and the line is gone -- silently, and precisely for the input most
 * worth having a log line about. A malformed byte in a request path is exactly the sort of thing
 * somebody reads the log to find out about.
 *
 * So the check happens here, per value, where the field is still known: the valid prefix goes
 * out followed by U+FFFD, which is what a log shipper would have put there and is itself valid
 * UTF-8. Everything after the first bad byte is dropped rather than scanned for more -- this is
 * a diagnostic, and one replacement character says what happened as well as five would.
 *
 * The patched copy is laid down in the same scratch arena the document is built in, so it needs
 * no buffer of its own and nothing has to free it.
 */
static void add_text(arnm_json_writer *w, arnm *scratch, const char *key, size_t key_length,
                     bool escape_key, const char *value, size_t length)
{
    const size_t valid = arnm_utf8_valid_length(value, length);
    const size_t patched_length = valid + sizeof(REPLACEMENT_CHARACTER) - 1u;
    uint8_t *patched = NULL;

    if (valid == length) {
        arnm_json_writer_add_string_flags(w, key, key_length, escape_key, value, length,
                                          ARNM_JSON_WRITER_STRING_ESCAPE);
        return;
    }
    if (arnm_alloc(&patched, (uint32_t)patched_length, scratch) != ARNM_SUCCESS) {
        /* No room for the copy: the valid prefix alone still passes the check, which is what
         * this is protecting. A shorter sentence beats a lost one. */
        arnm_json_writer_add_string_flags(w, key, key_length, escape_key, value, valid,
                                          ARNM_JSON_WRITER_STRING_ESCAPE);
        return;
    }
    memcpy(patched, value, valid);
    memcpy(patched + valid, REPLACEMENT_CHARACTER, sizeof(REPLACEMENT_CHARACTER) - 1u);
    arnm_json_writer_add_string_flags(w, key, key_length, escape_key, (const char *)patched,
                                      patched_length, ARNM_JSON_WRITER_STRING_ESCAPE);
}

/*
 * The line is laid down in the scratch arena and left there. What the caller does with it
 * differs: the logger thread copies it into its output buffer, because one contiguous write(2)
 * per flush is worth far more than the copy it costs -- a writev() over one segment per line
 * measures 53 ns a line against 165 for a single write of the same bytes. The synchronous path
 * has no buffer to fill and writes straight from here.
 *
 * The bytes are valid until the next call on this thread.
 */
arnm_result sc_log_encode_json(const sc_log_record *r, const uint8_t **text, uint32_t *out_length)
{
    arnm scratch;
    arnm_json_writer w;
    arnm_memory_block line;
    uint32_t len = 0;
    uint16_t i;
    arnm_result written;

    if (arnm_init_arena_borrow(&scratch, t_json_scratch, (uint32_t)sizeof(t_json_scratch)) !=
        ARNM_SUCCESS)
        return ARNM_ERROR_INVALID_STATE;
    /* The widest contracted line is 8 envelope fields, err{2} and data{8}: 18 members, so 36
     * values plus the two containers. Saying so up front spares the writer one chunk. */
    {
        static const arnm_json_writer_hint kHint = {40u, 0u};
        if (arnm_json_writer_init(&w, &scratch, ARNM_JSON_WRITE_NEWLINE_AT_END, &kHint) !=
            ARNM_SUCCESS)
            return ARNM_ERROR_INVALID_STATE;
    }

    arnm_json_writer_add_int64(&w, K("time"), r->time_ms);
    arnm_json_writer_add_uint64(&w, K("level"), r->level);
    ADD_LITERAL(&w, K("cat"),
                kScLogCatNames[r->cat < SC_CAT__COUNT ? r->cat : SC_CAT_STARTUP]);
    ADD_LITERAL(&w, K("event"), r->event);

    if (r->req != NULL)
        ADD_TEXT(&w, &scratch, K("req"), r->req, strlen(r->req));
    if (r->usr != 0)
        arnm_json_writer_add_uint64(&w, K("usr"), r->usr);
    if (r->err_name != NULL) {
        arnm_json_writer_open_object(&w, K("err"));
        arnm_json_writer_add_uint64(&w, K("code"), r->err_code);
        ADD_LITERAL(&w, K("name"), r->err_name);
        arnm_json_writer_close(&w);
    }
    if (r->data_count > 0) {
        arnm_json_writer_open_object(&w, K("data"));
        for (i = 0; i < r->data_count; ++i) {
            const sc_log_value *v = &r->data[i];
            /* a key from a SC_LOG_* macro: a literal, so it never needs escaping either */
            const size_t key_length = strlen(v->key);
            switch (v->kind) {
            case SC_LOG_VALUE_STRING: {
                /* the empty string and not `null`: the member was there and held nothing */
                const char *text = v->text != NULL ? v->text : "";
                add_text(&w, &scratch, v->key, key_length, false, text, strlen(text));
                break;
            }
            case SC_LOG_VALUE_INT:
                arnm_json_writer_add_int64(&w, v->key, key_length, false, v->number);
                break;
            case SC_LOG_VALUE_UINT:
                arnm_json_writer_add_uint64(&w, v->key, key_length, false, v->unumber);
                break;
            case SC_LOG_VALUE_BOOL:
                arnm_json_writer_add_bool(&w, v->key, key_length, false, v->number != 0);
                break;
            case SC_LOG_VALUE_NULL:
            default:
                arnm_json_writer_add_null(&w, v->key, key_length, false);
                break;
            }
        }
        arnm_json_writer_close(&w);
    }
    /* msg last, so a line read by a human ends with the sentence. */
    ADD_TEXT(&w, &scratch, K("msg"), r->msg, strlen(r->msg));

    /* One refusal anywhere above stops the writer and is answered here, so a line is written
     * whole or not at all -- there is no half a line to flush. Neither the text nor the
     * document is given back: the arena is this buffer, and the next line borrows it again. */
    written = arnm_json_writer_write(&w, &scratch, &line, &len);
    if (ARNM_SUCCESS != written)
        return written;
    *text = line.data;
    *out_length = len;
    return ARNM_SUCCESS;
}
