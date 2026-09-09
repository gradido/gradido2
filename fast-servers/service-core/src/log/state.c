#include "state.h"

#include <string.h>

#include <uv.h>

#include "ring.h"

/* The defaults, spelled twice: here as the static initialiser and in sc_log_default_config()
 * below, which is what a caller starts from. They have to agree -- a line written before
 * sc_log_init() reads this one, and it would be a poor answer if it meant something else. */
sc_log_config g_sc_log_cfg = {SC_LOG_INFO, 0, 2, -1, -1, 4096u, 64u, 64u * 1024u, 10000u, 1};

static _Atomic int g_sc_log_is_running;

_Atomic uint64_t g_sc_log_n_submitted;
_Atomic uint64_t g_sc_log_n_written;
_Atomic uint64_t g_sc_log_n_direct;
_Atomic uint64_t g_sc_log_n_blocked;
_Atomic uint64_t g_sc_log_n_flushes;
_Atomic uint32_t g_sc_log_peak_fill;

/* The wall clock, for the stamp on a record. uv_gettimeofday and not uv_hrtime: this one has
 * to mean a date, which a monotonic counter does not. */
int64_t sc_now_ms(void)
{
    /* Zeroed although uv_gettimeofday fills both fields on success: under LTO gcc looks into
     * it, cannot see that the two are tied to the return value, and warns. */
    uv_timeval64_t tv = {0, 0};
    if (uv_gettimeofday(&tv) != 0)
        return 0;
    return (int64_t)tv.tv_sec * 1000 + (int64_t)tv.tv_usec / 1000;
}

int sc_log_running(void)
{
    return atomic_load_explicit(&g_sc_log_is_running, memory_order_acquire);
}

void sc_log_set_running(int running)
{
    atomic_store_explicit(&g_sc_log_is_running, running, memory_order_release);
}

/* The monotonic one, for measuring a span. uv_hrtime counts nanoseconds from an unspecified
 * point, which is all a duration needs. */
double sc_log_mono_seconds(void)
{
    return (double)uv_hrtime() / 1e9;
}

void sc_log_default_config(sc_log_config *cfg)
{
    if (cfg == NULL)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->min_level = SC_LOG_INFO;
    cfg->synchronous = 0;
    cfg->json_fd = 2;
    cfg->pretty_fd = -1;
    cfg->pretty_color = -1;
    cfg->ring_capacity = 4096;
    cfg->spare_per_grade = 64;
    cfg->out_buffer_bytes = 64u * 1024u;
    cfg->write_stall_ms = 10000;
    cfg->fatal_on_disk_full = 1;
}

/* "info", "debug", ... onto a level. Here rather than beside sc_log() because it is read at
 * startup, out of the environment, and has nothing to do with writing a line. */
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

void sc_log_counters_reset(void)
{
    atomic_store(&g_sc_log_n_submitted, 0);
    atomic_store(&g_sc_log_n_written, 0);
    atomic_store(&g_sc_log_n_direct, 0);
    atomic_store(&g_sc_log_n_blocked, 0);
    atomic_store(&g_sc_log_n_flushes, 0);
    atomic_store(&g_sc_log_peak_fill, 0);
}

void sc_log_get_stats(sc_log_stats *out)
{
    if (out == NULL)
        return;
    out->submitted = atomic_load(&g_sc_log_n_submitted);
    out->written = atomic_load(&g_sc_log_n_written);
    out->direct = atomic_load(&g_sc_log_n_direct);
    out->blocked = atomic_load(&g_sc_log_n_blocked);
    out->flushes = atomic_load(&g_sc_log_n_flushes);
    out->peak_fill = atomic_load(&g_sc_log_peak_fill);
    out->ring_capacity = (uint32_t)sc_log_ring_capacity();
    out->host_allocs = sc_log_owner_host_allocs_total();
}
