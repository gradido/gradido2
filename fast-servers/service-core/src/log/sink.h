/*
 * Bytes onto one descriptor, and what to do when that fails.
 *
 * The whole policy sits here rather than at the call sites, because the answer to a failed
 * write is never local: a full disk is not a logging problem and has to stop the process,
 * while a reader that walked away is one and must not.
 */
#ifndef SERVICE_CORE_LOG_SINK_H
#define SERVICE_CORE_LOG_SINK_H

#include <stddef.h>

/*
 * One run of bytes out, retried until it is all gone.
 *
 *   ENOSPC/EDQUOT/EIO -> fatal, because a full disk is not a logging problem
 *   EPIPE             -> the reader is gone; the line is lost and the process runs on
 *   EINTR/EAGAIN      -> retry
 *
 * Plus the stall bound: a single write that takes longer than cfg.write_stall_ms means the
 * sink is stuck, and that is the bound without which the submitters' unbounded wait would
 * have none. A descriptor below zero and a length of zero are both no-ops.
 */
void sc_log_sink_write(int fd, const char *data, size_t len);

#endif /* SERVICE_CORE_LOG_SINK_H */
