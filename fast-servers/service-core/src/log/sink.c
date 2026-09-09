#include "sink.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include <uv.h>

#include "service_core/log/log.h"
#include "state.h"

/*
 * One run of bytes onto one descriptor, through uv_fs_write with a NULL callback -- which is
 * how libuv spells a synchronous call, and what makes this the same blocking write it was
 * while it was write(2), on a platform where write(2) is not spelled that way.
 *
 * The errors come back as negative UV_E* rather than through errno, which reads the same
 * except for one: libuv's table has no UV_EDQUOT. On a unix host that is no loss, because
 * libuv hands an unmapped errno straight back as its own negative -- so the check below is by
 * -EDQUOT where the platform has the name at all. Windows has no disk quota errno and the
 * case simply cannot arise there.
 */
static void sc_log_sink_fatal(const char *why, int err)
{
    char line[512];
    int n = snprintf(line, sizeof(line),
                     "{\"time\":%lld,\"level\":60,\"cat\":\"startup\",\"event\":\"log.sink.fatal\""
                     ",\"data\":{\"reason\":\"%s\",\"errno\":%d},\"msg\":\"log sink failed, "
                     "aborting\"}\n",
                     (long long)sc_now_ms(), why, err);
    if (n > 0) {
        uv_fs_t req;
        uv_buf_t buf = uv_buf_init(line, (unsigned int)n);
        (void)uv_fs_write(NULL, &req, 2, &buf, 1, -1, NULL);
        uv_fs_req_cleanup(&req);
    }
    abort();
}

/* Whether this is the disk saying it is full rather than a reader that walked away. */
static int is_out_of_space(int err)
{
    if (err == UV_ENOSPC)
        return 1;
#ifdef EDQUOT
    if (err == -EDQUOT)
        return 1;
#endif
    return 0;
}

void sc_log_sink_write(int fd, const char *data, size_t len)
{
    size_t done = 0;
    if (fd < 0 || len == 0)
        return;
    while (done < len) {
        size_t left = len - done;
        unsigned int chunk = left > (size_t)UINT_MAX ? UINT_MAX : (unsigned int)left;
        /* uv_buf_t carries a writable pointer because the same type describes a read. The
         * write path only reads from it. */
        uv_buf_t buf = uv_buf_init((char *)(uintptr_t)(const void *)(data + done), chunk);
        uv_fs_t req;
        double t0 = sc_log_mono_seconds();
        int n = uv_fs_write(NULL, &req, fd, &buf, 1, -1, NULL);
        double dt = sc_log_mono_seconds() - t0;

        uv_fs_req_cleanup(&req);

        if (dt * 1000.0 > (double)g_sc_log_cfg.write_stall_ms)
            sc_log_sink_fatal("write_stalled", 0);

        if (n > 0) {
            done += (size_t)n;
            continue;
        }
        if (n == UV_EINTR || n == UV_EAGAIN)
            continue;
        if (is_out_of_space(n) || n == UV_EIO) {
            if (g_sc_log_cfg.fatal_on_disk_full)
                sc_log_sink_fatal(n == UV_EIO ? "io_error" : "no_space", n);
            return;
        }
        return; /* UV_EPIPE and everything else: the line is lost, the process runs on */
    }
    atomic_fetch_add_explicit(&g_sc_log_n_flushes, 1, memory_order_relaxed);
}
