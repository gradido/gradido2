/*
 * The two encoders.
 *
 * Both turn one sc_log_record into bytes and neither knows where those bytes go: the drain
 * thread packs them into a buffer, the synchronous path writes them straight out. They share
 * nothing with each other either -- the JSON side goes through arnm/json_writer.h, the pretty
 * side lays out characters itself -- which is why they live in two files behind one header.
 */
#ifndef SERVICE_CORE_LOG_FORMAT_H
#define SERVICE_CORE_LOG_FORMAT_H

#include <stddef.h>
#include <stdint.h>

#include "arnm/byte_buffer.h"
#include "arnm/result.h"
#include "record.h"

/*
 * The contracted line, laid down in this thread's scratch arena and left there.
 *
 * @p text and @p out_length receive the bytes and their length. They stay valid until the next
 * call on this thread, which is enough for both callers: the drain thread copies them into its
 * output buffer before the next line, and the synchronous path writes them and is done.
 *
 * Answers what the writer answered. A line is written whole or not at all -- one refusal
 * anywhere stops the writer -- so there is never half a line to flush.
 */
arnm_result sc_log_encode_json(const sc_log_record *r, const uint8_t **text, uint32_t *out_length);

/*
 * What sc_log_pretty_measure() worked out about one record, so that sc_log_encode_pretty() need
 * not ask again: strlen runs once per string rather than twice, and the converter's size step
 * once per number -- the write pass hands that size straight back as the size it already knows.
 */
typedef struct sc_log_pretty_plan {
    size_t total;
    uint32_t level_color, level, cat, event, msg, req, err_name;
    uint8_t usr, err_code;
    uint32_t key[SC_LOG_DATA_MAX];
    uint32_t val[SC_LOG_DATA_MAX]; /* the rendered value, whatever its kind spells */
} sc_log_pretty_plan;

/*
 * The exact number of bytes sc_log_encode_pretty() will write for this record, and the plan it
 * needs to write them. Not a bound and not an estimate -- see the comment on the definition for
 * why a pretty line cannot have a constant one.
 */
size_t sc_log_pretty_measure(sc_log_pretty_plan *p, const sc_log_record *r, int color);

/*
 * The human line. Every append inside is unchecked, so the caller must have asked
 * sc_log_pretty_measure() for the length and arnm_byte_buffer_available() for the room, and must
 * not call this unless the second covers the first.
 */
void sc_log_encode_pretty(arnm_byte_buffer *b, const sc_log_pretty_plan *p,
                          const sc_log_record *r, int color);

#endif /* SERVICE_CORE_LOG_FORMAT_H */
