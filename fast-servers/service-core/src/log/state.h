/*
 * The settings in force and the tallies.
 *
 * Neither belongs to any one part of the logger: the submitting threads read the level, the
 * sink reads the stall bound, the drain thread reads the descriptors, and every one of them
 * counts something. Rather than let whichever file writes them most claim them, they sit here.
 *
 * g_sc_log_cfg is written once, by sc_log_init(), before the logger thread exists; everything
 * after that only reads it. It carries the defaults as its static initialiser rather than
 * being filled in by sc_log_init() alone, so that a line written before any logger exists --
 * sc_config_load() reporting a broken environment, a unit test that linked this component and
 * arranged nothing -- still finds a level and a descriptor that mean something. See
 * sc_log_running() below.
 *
 * The counters are atomic because several threads add to them at once, and relaxed because
 * nothing is ordered against them -- they are a report, not a protocol.
 */
#ifndef SERVICE_CORE_LOG_STATE_H
#define SERVICE_CORE_LOG_STATE_H

#include <stdatomic.h>
#include <stdint.h>

#include "service_core/log/logger.h"

extern sc_log_config g_sc_log_cfg;

extern _Atomic uint64_t g_sc_log_n_submitted;
extern _Atomic uint64_t g_sc_log_n_written;
extern _Atomic uint64_t g_sc_log_n_direct;
extern _Atomic uint64_t g_sc_log_n_blocked;
extern _Atomic uint64_t g_sc_log_n_flushes;
extern _Atomic uint32_t g_sc_log_peak_fill;

/**
 * Whether a logger thread is running and there is a ring to hand a record to.
 *
 * The whole submitting side turns on it: before sc_log_init() and after sc_log_shutdown()
 * there is no ring to reserve a slot in and no thread to drain one, so a line takes the
 * synchronous path instead of reaching for either. Acquire, because what it publishes is the
 * ring the reader is about to use.
 */
int sc_log_running(void);

/** Says a logger is (not) running. sc_log_init() and sc_log_shutdown() call it; nothing else. */
void sc_log_set_running(int running);

/** Seconds off the monotonic clock -- for measuring a span, never for stamping a record. */
double sc_log_mono_seconds(void);

/** Puts every tally back to zero. sc_log_init() calls it; nothing else should. */
void sc_log_counters_reset(void);

#endif /* SERVICE_CORE_LOG_STATE_H */
