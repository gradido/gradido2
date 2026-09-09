/*
 * The drain thread, and the two buffers it fills.
 *
 * One line at a time, in ticket order: encode it, pack it into the buffer for its stream, put
 * the arena back into its owner's return queue -- and never free it, that is the owner's to do.
 * The buffers belong to this thread alone, which is why nothing here takes a lock.
 *
 * This file is also where the logger is composed: sc_log_init() normalises the configuration,
 * makes the ring and the buffers, and starts the thread; sc_log_shutdown() unmakes all of it.
 */
#include "service_core/log/logger.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "arnm/byte_buffer.h"
#include "format.h"
#include "record.h"
#include "ring.h"
#include "sink.h"
#include "state.h"

/* Both streams pack finished lines back to back and hand each run to one write(); that is what
 * arnm_byte_buffer is, so it does the bookkeeping instead of us. */
static arnm_byte_buffer g_out;
static arnm_byte_buffer g_pretty;
static int g_pretty_color;

static uv_thread_t g_thread;
static int g_thread_started;
static int g_warned_at[3]; /* 50 %, 75 %, 90 % -- once each */

static void out_flush(void)
{
    const uint8_t *bytes = NULL;
    uint32_t length = 0;
    if (ARNM_SUCCESS == arnm_byte_buffer_access(&g_out, &bytes, &length) && length > 0) {
        sc_log_sink_write(g_sc_log_cfg.json_fd, (const char *)bytes, length);
        arnm_byte_buffer_clear(&g_out);
    }
    if (ARNM_SUCCESS == arnm_byte_buffer_access(&g_pretty, &bytes, &length) && length > 0) {
        sc_log_sink_write(g_sc_log_cfg.pretty_fd, (const char *)bytes, length);
        arnm_byte_buffer_clear(&g_pretty);
    }
}

static void note_fill(uint64_t fill)
{
    const uint64_t capacity = sc_log_ring_capacity();
    static const int kMarks[3] = {50, 75, 90};
    uint32_t peak = atomic_load_explicit(&g_sc_log_peak_fill, memory_order_relaxed);
    int i;
    if ((uint32_t)fill > peak)
        atomic_store_explicit(&g_sc_log_peak_fill, (uint32_t)fill, memory_order_relaxed);
    for (i = 0; i < 3; ++i) {
        if (!g_warned_at[i] && fill * 100u >= capacity * (uint64_t)kMarks[i]) {
            sc_log_value data[2] = {SC_LOG_INT("percent", kMarks[i]),
                                   SC_LOG_INT("capacity", (int64_t)capacity)};
            sc_log_context ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.data = data;
            ctx.data_count = 2;
            g_warned_at[i] = 1;
            /* Around the ring -- the ring is the bottleneck right now. */
            sc_log_direct(SC_LOG_WARN, SC_CAT_STARTUP, "log.ring.highwater", &ctx,
                         "log ring reached %d%% of %llu slots, consider raising ring_capacity",
                         kMarks[i], (unsigned long long)capacity);
        }
    }
}

/* uv_thread_cb answers nothing, unlike pthread's start routine -- there is no return value to
 * drop here, and nothing waits on one: sc_log_shutdown() joins the thread and reads the state it
 * left behind. */
static void logger_main(void *arg)
{
    sc_log_slot *s;
    (void)arg;

    while (sc_log_ring_take(&s, out_flush)) {
        note_fill(sc_log_ring_fill());

        {
            const uint8_t *text = NULL;
            uint32_t len = 0;
            /* The writer hands back the length it wrote, so the room question is answered
             * rather than guessed -- see sc_log_pretty_measure() for why no constant can answer it.
             * The text sits in this thread's own scratch and outlives the flush, which only
             * empties g_out. A line wider than the whole buffer is refused entire; half a
             * record in a packed stream reads as a short one. */
            if (sc_log_encode_json(s->rec, &text, &len) == ARNM_SUCCESS) {
                if (arnm_byte_buffer_available(&g_out) < len)
                    out_flush();
                arnm_byte_buffer_copy(&g_out, text, len);
            }
        }

        if (g_sc_log_cfg.pretty_fd >= 0) {
            sc_log_pretty_plan plan;
            size_t want = sc_log_pretty_measure(&plan, s->rec, g_pretty_color);
            if (arnm_byte_buffer_available(&g_pretty) < want)
                out_flush();
            /* This is the room the unchecked appends inside sc_log_encode_pretty() stand on. A line
             * wider than the buffer itself is dropped whole, the way the JSON side drops one:
             * a cut line in a human stream reads as a complete short one. */
            if (arnm_byte_buffer_available(&g_pretty) >= want)
                sc_log_encode_pretty(&g_pretty, &plan, s->rec, g_pretty_color);
        }

        /* Warn and worse go straight through -- those are the lines one opens the log for
         * after a crash. */
        if (s->rec->level >= SC_LOG_WARN)
            out_flush();

        sc_log_owner_return(sc_log_owner_at((int)s->owner), s->arena);
        atomic_fetch_add_explicit(&g_sc_log_n_written, 1, memory_order_relaxed);
        sc_log_ring_done(s);
    }
    out_flush();
}

/* ------------------------------------------------------------------ Start and stop */

int sc_log_init(const sc_log_config *cfg)
{
    if (g_thread_started)
        return -1;
    if (cfg != NULL)
        g_sc_log_cfg = *cfg;
    else
        sc_log_default_config(&g_sc_log_cfg);
    if (g_sc_log_cfg.out_buffer_bytes == 0)
        g_sc_log_cfg.out_buffer_bytes = 64u * 1024u;
    if (g_sc_log_cfg.write_stall_ms == 0)
        g_sc_log_cfg.write_stall_ms = 10000;
    if (g_sc_log_cfg.out_buffer_bytes < SC_LOG_GRADE_MAX * 2u)
        g_sc_log_cfg.out_buffer_bytes = SC_LOG_GRADE_MAX * 2u;

    /* Asked for the synchronous mode: the configuration is now in force -- which is what makes
     * the level and the descriptors count -- and there is nothing else to build. Every line
     * takes the path it takes before any sc_log_init() at all. */
    if (g_sc_log_cfg.synchronous)
        return 0;

    if (sc_log_ring_init(g_sc_log_cfg.ring_capacity) != 0)
        return -1;

    /* NULL allocator: the host's. Each run is written out and cleared, never grown, so this is
     * the only time anything is asked for either of them. */
    if (ARNM_SUCCESS != arnm_byte_buffer_init(&g_out, g_sc_log_cfg.out_buffer_bytes, NULL) ||
        (g_sc_log_cfg.pretty_fd >= 0 &&
         ARNM_SUCCESS != arnm_byte_buffer_init(&g_pretty, g_sc_log_cfg.out_buffer_bytes, NULL))) {
        sc_log_ring_free();
        arnm_byte_buffer_free(&g_out, NULL);
        arnm_byte_buffer_free(&g_pretty, NULL);
        return -1;
    }
    /* uv_guess_handle instead of isatty: the same question, asked where Windows can answer it
     * too -- there a console is a handle kind, not a file descriptor property. */
    g_pretty_color = g_sc_log_cfg.pretty_color >= 0
                         ? g_sc_log_cfg.pretty_color
                         : (g_sc_log_cfg.pretty_fd >= 0 &&
                            uv_guess_handle(g_sc_log_cfg.pretty_fd) == UV_TTY);

    memset(g_warned_at, 0, sizeof(g_warned_at));
    sc_log_counters_reset();

    if (uv_thread_create(&g_thread, logger_main, NULL) != 0) {
        sc_log_ring_free();
        arnm_byte_buffer_free(&g_out, NULL);
        arnm_byte_buffer_free(&g_pretty, NULL);
        return -1;
    }
    g_thread_started = 1;
    /* Last, and only once every part above exists: this is the flag the submitting side reads
     * to decide whether there is a ring to hand a record to at all. */
    sc_log_set_running(1);
    return 0;
}

/*
 * The counterpart, and it has to be the last thing the process does with the logger: a thread
 * still logging while this runs would find the ring gone underneath it. Every role thread is
 * joined before main reaches this, which is what makes that a rule rather than a race.
 */
void sc_log_shutdown(void)
{
    if (!g_thread_started)
        return;
    /* First, so that a line written from here on takes the synchronous path rather than
     * reaching for a ring that is about to be freed. */
    sc_log_set_running(0);
    sc_log_ring_stop();
    (void)uv_thread_join(&g_thread);
    g_thread_started = 0;
    sc_log_ring_free();
    arnm_byte_buffer_free(&g_out, NULL);
    arnm_byte_buffer_free(&g_pretty, NULL);
    /*
     * Back to the defaults, and the descriptors are the reason: they belong to whoever opened
     * them, and after this they may well be closed and the numbers handed to something else. A
     * line written from here on takes the synchronous path -- see logger.h -- and that path
     * must find stderr rather than whatever now sits where the JSON stream used to be.
     */
    sc_log_default_config(&g_sc_log_cfg);
}
