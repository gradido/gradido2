/*
 * The record as it lies in the arena, and the two tables that go with it.
 *
 * This is the vocabulary the submitting side and the two encoders share, and the only thing
 * they share: submit() builds one of these, the encoders read one, and neither knows anything
 * else about the other.
 */
#ifndef SERVICE_CORE_LOG_RECORD_H
#define SERVICE_CORE_LOG_RECORD_H

#include <stdint.h>

#include "service_core/log/log.h"

/*
 * Layout of one record, all of it a single block out of one arena:
 *
 *   [ sc_log_record ][ sc_log_value * data_count ][ msg\0 ][ req\0 ][ text\0 ... ]
 *
 * `event`, `err_name` and the `data` keys are string literals and stay pointers. Everything
 * else is copied, because the caller's stack frame is gone by the time the logger reads it.
 */
typedef struct sc_log_record {
    int64_t time_ms;
    uint64_t seq;
    const char *event;
    const char *err_name;
    const char *msg;
    const char *req;
    uint64_t usr;
    uint32_t err_code;
    uint16_t data_count;
    uint8_t level;
    uint8_t cat;
    sc_log_value *data;
} sc_log_record;

/** The names the `cat` field stands for, in the order of the enum. */
extern const char *const kScLogCatNames[SC_CAT__COUNT];

/* The ladder of grades. A typical line needs ~300 bytes, the widest ~2 KiB. */
#define SC_LOG_GRADE_COUNT ((uint16_t)6)
#define SC_LOG_GRADE_MAX 4096u
extern const uint32_t kScLogGrades[SC_LOG_GRADE_COUNT];

#endif /* SERVICE_CORE_LOG_RECORD_H */
