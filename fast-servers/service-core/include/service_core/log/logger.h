/*
 * Starting and stopping the logger, and what a thread owes it while it runs.
 *
 * `log.h` beside this file is the line -- the levels, the categories, the envelope and the
 * five calls that write one. This is the other half: the process-wide arrangement that makes
 * those calls cost 74 ns instead of 4776. Only main and the tests include it.
 *
 * ### The shape in one paragraph
 *
 * Every submitting thread owns an `arnm_graded_arena_pool` and a return queue of its own. It
 * takes an arena, builds the record and every copied string inside it, and puts
 * {arena, record, owner} into a shared MPSC ring. One logger thread pulls from that ring,
 * encodes the contracted JSON (and optionally a pino-pretty style line beside it), and puts
 * the arena into its owner's return queue -- it never touches a pool it does not own, because
 * arnm says so: *"One pool used from two threads at once is a data race. A pool per thread is
 * the intended shape."* The owner collects what came back on its next log call, which is the
 * only place `pool_free` is ever called.
 *
 * ### Backpressure, and what bounds it
 *
 * When the ring is full the submitter **waits**, without a bound. Nothing is ever dropped. If
 * logging cannot keep up the server is meant to slow down, and a database or session lock held
 * while waiting is not a problem here: the logger thread needs neither, so there is no cycle --
 * only a queue.
 *
 * What that does not cure is a *stalled* sink, and the bound for it sits with the logger rather
 * than with the submitter: a single write that takes longer than @ref sc_log_config::write_stall_ms
 * writes the reason straight to stderr and aborts the process. Likewise ENOSPC, EDQUOT and EIO,
 * unless @ref sc_log_config::fatal_on_disk_full says otherwise. EPIPE is not fatal -- there the
 * reader is simply gone.
 *
 * ### What a caller owes it
 *
 * A thread that logs registers once with sc_log_thread_join() and gives its pool back with
 * sc_log_thread_leave() before it ends. Forgetting the first costs nothing -- the first line
 * registers the thread itself -- but forgetting the second leaks one pool and one return queue
 * for the life of the process, which for a thread that comes and goes (an email worker) is a
 * leak per retirement. sc_log_thread_leave() must run **while the logger is still running**:
 * it waits for every arena of that thread to come back, and it is the logger that hands them
 * back.
 *
 * ### Before sc_log_init, and after sc_log_shutdown
 *
 * A line written when no logger is running is not lost and does not crash: it takes the
 * synchronous path of sc_log_direct() at the default configuration -- the contracted JSON, on
 * stderr, at info and above. That is what lets sc_config_load() report a broken environment
 * before anything has been started, and what lets a unit test link this component without
 * arranging a logger first.
 */
#ifndef SERVICE_CORE_LOG_LOGGER_H
#define SERVICE_CORE_LOG_LOGGER_H

#include <stdint.h>

#include "service_core/log/log.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Submitting threads that may be registered at the same time. */
#define SC_LOG_MAX_THREADS 64

typedef struct sc_log_config {
    /** Lowest level that is emitted. */
    sc_log_level min_level;
    /**
     * Write every line on the calling thread instead of starting a logger: no ring, no drain
     * thread, one write per line.
     *
     * For a short-lived command rather than for a server. `setup` asks questions on a terminal
     * and the answer to a question must not arrive between a line and the buffer it is sitting
     * in; `migrate-down` writes a handful of lines and stops. Neither has any throughput to
     * protect, and the ring exists to protect throughput. It is the same call the reference path
     * makes with pino's `sync` when stdout is a terminal.
     *
     * sc_log_init() still stores the configuration, so the level is honoured; it simply builds
     * nothing. Everything else on the surface keeps working: sc_log_thread_join() answers -1,
     * sc_log_thread_leave() does nothing, and sc_log_shutdown() has nothing to stop.
     */
    int synchronous;
    /**
     * Descriptor for the JSON stream -- the contract stream, this is what tests parse.
     *
     * 2 by default, and stderr rather than stdout for the reason the C path has always had:
     * the roles share the stream, and a log line must not end up mixed into whatever a future
     * subcommand writes for a human to read.
     */
    int json_fd;
    /** Descriptor for pino-pretty style output, or -1. Never the same as @ref json_fd. */
    int pretty_fd;
    /** Force colour in the pretty stream; -1 asks uv_guess_handle whether pretty_fd is a tty. */
    int pretty_color;
    /** Slots in the ring. Rounded up to a power of two, at least 8. */
    uint32_t ring_capacity;
    /**
     * Arenas each grade creates per thread at startup. The burst that goes through without a
     * single malloc. The free list's ceiling is independent of this and is always the ring
     * capacity -- a returned arena is never handed back to the host while the ring could still
     * ask for it again.
     */
    uint32_t spare_per_grade;
    /** Size of the logger thread's output buffer. 0 means 64 KiB. */
    uint32_t out_buffer_bytes;
    /**
     * If a single write takes longer than this, the sink counts as stalled and the process
     * aborts -- the bound without which the submitters' unbounded wait would have none at all.
     * 0 means 10000.
     */
    uint32_t write_stall_ms;
    /** Abort on ENOSPC/EDQUOT/EIO. 1 = yes (default), 0 = report and carry on. */
    int fatal_on_disk_full;
} sc_log_config;

/** Fills @p cfg with the defaults. The starting point, so later fields cannot surprise. */
void sc_log_default_config(sc_log_config *cfg);

/**
 * Starts the logger thread. Once, before any other thread. 0 on success.
 *
 * @p cfg may be NULL, which is the defaults. A second call while one is running answers -1 and
 * changes nothing. With @ref sc_log_config::synchronous set it starts nothing and answers 0.
 */
int sc_log_init(const sc_log_config *cfg);

/**
 * Registers the calling thread: its own arena pool, its own return queue.
 * Returns the owner id, or -1 when no logger is running or all slots are taken.
 */
int sc_log_thread_join(void);

/**
 * Unregisters the thread. Waits until every arena of its has come back from the logger, then
 * releases the pool. Must run **while the logger is still running**.
 */
void sc_log_thread_leave(void);

/** Stops, drains the ring, flushes the buffers, joins the thread. */
void sc_log_shutdown(void);

/** Counters, for the tests and for the logger's view of itself. */
typedef struct sc_log_stats {
    uint64_t submitted;     /**< lines handed to the ring */
    uint64_t written;       /**< lines the logger has written */
    uint64_t direct;        /**< lines written around the ring */
    uint64_t blocked;       /**< how often a submitter had to wait on a full ring */
    uint64_t flushes;       /**< write calls the logger made */
    /** Highest number of tickets reserved but not yet consumed. May exceed the capacity by
     *  the number of waiting submitters. */
    uint32_t peak_fill;
    uint32_t ring_capacity; /**< slots in the ring */
    uint64_t host_allocs;   /**< arena requests that found the stock empty */
} sc_log_stats;

void sc_log_get_stats(sc_log_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* SERVICE_CORE_LOG_LOGGER_H */
