/*
 * Outbound HTTP: one GET, its whole body into a caller's buffer. curl underneath, the library the
 * mailer already links -- build.zig, *libcurl*. curl_global_init has run in main before any role
 * starts.
 */
#ifndef SERVICE_CORE_HTTP_CLIENT_H
#define SERVICE_CORE_HTTP_CLIENT_H

#include <stddef.h>

#include "service_core/runtime.h"
#include "service_core/status.h"

/**
 * GETs @p url and writes the body into @p out, its length into @p len and the HTTP status into
 * @p http_status. Gives up after @p timeout_ms, and at once when @p quit is raised (it may be NULL).
 *
 * SC_ERR_NETWORK: no answer -- refused, timed out, TLS failed, or quit. SC_ERR_TOO_LONG: the body
 * did not fit into @p cap bytes. Any status is SC_OK; what a 404 means is the caller's.
 */
sc_status sc_http_get(const char *url, long timeout_ms, const sc_quit_flag *quit, char *out,
                      size_t cap, size_t *len, long *http_status);

#endif /* SERVICE_CORE_HTTP_CLIENT_H */
