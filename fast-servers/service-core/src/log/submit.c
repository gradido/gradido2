/*
 * What a caller touches.
 *
 * sc_log() and sc_log_event() format the sentence on the caller's stack, pack the record into an
 * arena out of this thread's own pool, and hand it to the ring. Everything after that belongs
 * to the drain thread. sc_log_direct() and sc_log_panic() go around all of it and write on the
 * spot, for the lines one opens the log for after a crash.
 *
 * The two paths are also what makes a line before sc_log_init() harmless: with no ring to hand
 * a record to, sc_log() writes it the way sc_log_direct() does. sc_config_load() reports a
 * broken environment that way, and a unit test that linked this component and arranged no
 * logger gets its lines rather than a crash.
 */
#include "service_core/log/logger.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arnm/dynamic_arena_pool.h"
#include "arnm/graded_arena_pool.h"
#include "arnm/memory.h"
#include "format.h"
#include "record.h"
#include "ring.h"
#include "sink.h"
#include "state.h"

int sc_log_thread_join(void)
{
    int owner = sc_log_owner_index();
    if (owner >= 0)
        return owner;
    /* Nothing to join: the pool's ceiling and its return queue are both sized from the ring,
     * and there is no ring yet. A line written now takes the synchronous path anyway. */
    if (!sc_log_running())
        return -1;
    return sc_log_owner_join();
}

void sc_log_thread_leave(void) { sc_log_owner_leave(); }

/* Both defined below; sc_log() and sc_log_event() choose between them per line. */
static void submit_v(sc_log_level level, sc_log_cat cat, const char *event,
                     const sc_log_context *ctx, const char *fmt, va_list ap);
static void direct_v(sc_log_level level, sc_log_cat cat, const char *event,
                     const sc_log_context *ctx, const char *fmt, va_list ap);

/* ------------------------------------------------------------------ Packing */

static size_t cstr_len(const char *s) { return s != NULL ? strlen(s) : 0; }

/* Copies @p src to where @p cursor points and advances the cursor past it. */
static const char *pack(char **cursor, const char *src, size_t len)
{
    char *dst = *cursor;
    memcpy(dst, src, len);
    dst[len] = '\0';
    *cursor = dst + len + 1;
    return dst;
}

static void submit(sc_log_level level, sc_log_cat cat, const char *event,
                   const sc_log_context *ctx, const char *msg, size_t msg_len)
{
    sc_log_owner *o;
    arnm *arena = NULL;
    uint8_t *block = NULL;
    sc_log_record *rec;
    char *cursor;
    size_t need;
    size_t i;
    size_t count = (ctx != NULL && ctx->data != NULL) ? ctx->data_count : 0;
    uint64_t pos;
    sc_log_slot *s;
    int owner = sc_log_owner_index();

    if (owner < 0) {
        owner = sc_log_owner_join();
        if (owner < 0)
            return;
    }
    o = sc_log_owner_at(owner);

    /* Collect our own returns first -- no lock, no foreign thread involved. */
    sc_log_owner_reclaim(o);

    if (count > SC_LOG_DATA_MAX)
        count = SC_LOG_DATA_MAX;

    need = sizeof(sc_log_record) + count * sizeof(sc_log_value) + msg_len + 1;
    if (ctx != NULL && ctx->req != NULL)
        need += cstr_len(ctx->req) + 1;
    for (i = 0; i < count; ++i)
        if (ctx->data[i].kind == SC_LOG_VALUE_STRING)
            need += cstr_len(ctx->data[i].text) + 1;

    if (need > SC_LOG_GRADE_MAX) {
        /* Only the sentence may be truncated; the structure never is. */
        size_t over = need - SC_LOG_GRADE_MAX;
        if (over >= msg_len)
            return;
        msg_len -= over;
        need = SC_LOG_GRADE_MAX;
    }

    {
        arnm_dynamic_arena_pool *grade;
        uint32_t cap = arnm_graded_arena_pool_capacity_for(&o->pool, (uint32_t)need);
        uint16_t g;
        for (g = 0; g < SC_LOG_GRADE_COUNT; ++g) {
            grade = arnm_graded_arena_pool_grade_at(&o->pool, g);
            if (grade != NULL && grade->arena_capacity == cap) {
                if (grade->spare_count == 0)
                    ++o->host_allocs;
                break;
            }
        }
    }
    if (arnm_graded_arena_pool_alloc(&o->pool, (uint32_t)need, &arena) != ARNM_SUCCESS)
        return;
    if (arnm_alloc(&block, (uint32_t)need, arena) != ARNM_SUCCESS) {
        (void)arnm_graded_arena_pool_free(&o->pool, arena);
        return;
    }

    rec = (sc_log_record *)(void *)block;
    rec->data = count > 0 ? (sc_log_value *)(void *)(block + sizeof(sc_log_record)) : NULL;
    cursor = (char *)(block + sizeof(sc_log_record) + count * sizeof(sc_log_value));

    rec->time_ms = sc_now_ms();
    rec->level = (uint8_t)level;
    rec->cat = (uint8_t)cat;
    rec->event = event;           /* literal */
    rec->usr = ctx != NULL ? ctx->usr : 0;
    rec->err_name = ctx != NULL ? ctx->err_name : NULL; /* literal */
    rec->err_code = ctx != NULL ? ctx->err_code : 0;
    rec->data_count = (uint16_t)count;
    rec->msg = pack(&cursor, msg, msg_len);
    rec->req = (ctx != NULL && ctx->req != NULL)
                   ? pack(&cursor, ctx->req, cstr_len(ctx->req))
                   : NULL;
    for (i = 0; i < count; ++i) {
        rec->data[i] = ctx->data[i];
        if (ctx->data[i].kind == SC_LOG_VALUE_STRING) {
            const char *t = ctx->data[i].text != NULL ? ctx->data[i].text : "";
            rec->data[i].text = pack(&cursor, t, strlen(t));
        }
    }

    /* Take a ticket and wait out the backpressure, then hand the record over. */
    s = sc_log_ring_reserve(&pos);
    rec->seq = pos;
    s->arena = arena;
    s->rec = rec;
    s->owner = (uint32_t)owner;
    sc_log_ring_publish(pos);
}

static void submit_v(sc_log_level level, sc_log_cat cat, const char *event,
                     const sc_log_context *ctx, const char *fmt, va_list ap)
{
    char msg[SC_LOG_MSG_MAX];
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);

    if (n < 0)
        return;
    submit(level, cat, event, ctx, msg, (size_t)n < sizeof(msg) ? (size_t)n : sizeof(msg) - 1);
}

void sc_log(sc_log_level level, sc_log_cat cat, const char *event, const char *fmt, ...)
{
    va_list ap;

    if (level < g_sc_log_cfg.min_level)
        return;
    va_start(ap, fmt);
    if (sc_log_running())
        submit_v(level, cat, event, NULL, fmt, ap);
    else
        direct_v(level, cat, event, NULL, fmt, ap);
    va_end(ap);
}

void sc_log_event(sc_log_level level, sc_log_cat cat, const char *event, const sc_log_context *ctx,
                 const char *fmt, ...)
{
    va_list ap;

    if (level < g_sc_log_cfg.min_level)
        return;
    va_start(ap, fmt);
    if (sc_log_running())
        submit_v(level, cat, event, ctx, fmt, ap);
    else
        direct_v(level, cat, event, ctx, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ Around the ring */

static void direct_v(sc_log_level level, sc_log_cat cat, const char *event,
                     const sc_log_context *ctx, const char *fmt, va_list ap)
{
    char msg[SC_LOG_MSG_MAX];
    const uint8_t *text = NULL;
    uint32_t len = 0;
    sc_log_record rec;
    sc_log_value values[SC_LOG_DATA_MAX];
    size_t count = (ctx != NULL && ctx->data != NULL) ? ctx->data_count : 0;
    size_t i;
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);

    if (n < 0)
        return;
    if (count > SC_LOG_DATA_MAX)
        count = SC_LOG_DATA_MAX;
    for (i = 0; i < count; ++i)
        values[i] = ctx->data[i];

    memset(&rec, 0, sizeof(rec));
    rec.time_ms = sc_now_ms();
    rec.level = (uint8_t)level;
    rec.cat = (uint8_t)cat;
    rec.event = event;
    rec.msg = msg;
    rec.req = ctx != NULL ? ctx->req : NULL;
    rec.usr = ctx != NULL ? ctx->usr : 0;
    rec.err_name = ctx != NULL ? ctx->err_name : NULL;
    rec.err_code = ctx != NULL ? ctx->err_code : 0;
    rec.data = count > 0 ? values : NULL;
    rec.data_count = (uint16_t)count;

    if (sc_log_encode_json(&rec, &text, &len) != ARNM_SUCCESS)
        return;
    /* One write, synchronous, straight from where the writer left the line: without the ring,
     * without the logger's output buffer and without a buffer of this frame's own. */
    sc_log_sink_write(g_sc_log_cfg.json_fd, (const char *)text, len);
    atomic_fetch_add_explicit(&g_sc_log_n_direct, 1, memory_order_relaxed);
}

void sc_log_direct(sc_log_level level, sc_log_cat cat, const char *event, const sc_log_context *ctx,
                  const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    direct_v(level, cat, event, ctx, fmt, ap);
    va_end(ap);
}

void sc_log_panic(sc_log_cat cat, const char *event, const sc_log_context *ctx, const char *fmt,
                  ...)
{
    va_list ap;
    va_start(ap, fmt);
    direct_v(SC_LOG_FATAL, cat, event, ctx, fmt, ap);
    va_end(ap);
    abort();
}
