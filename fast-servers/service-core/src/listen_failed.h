/*
 * The one line a server that cannot take its port is reported with:
 * `contracts/logging.json`, `startup.server.failed`. Both HTTP backends write it, so that
 * which of them is underneath cannot be read off the log.
 *
 * @p reason is one of the contract's closed vocabulary -- "address-in-use",
 * "address-invalid", "other". `packages/backend/src/main.ts` writes the same line on the
 * reference path.
 */
#ifndef SERVICE_CORE_LISTEN_FAILED_H
#define SERVICE_CORE_LISTEN_FAILED_H

#include "service_core/log/log.h"

#define SC_LISTEN_FAILED(port, reason, ...)                                                       \
    do {                                                                                          \
        sc_log_value sc_listen_failed_data[2] = {SC_LOG_UINT("port", (port)),                     \
                                                 SC_LOG_STR("reason", (reason))};                 \
        sc_log_context sc_listen_failed_context = {0};                                            \
                                                                                                  \
        sc_listen_failed_context.data = sc_listen_failed_data;                                    \
        sc_listen_failed_context.data_count = 2;                                                  \
        sc_log_event(SC_LOG_FATAL, SC_CAT_STARTUP, "startup.server.failed",                       \
                     &sc_listen_failed_context, __VA_ARGS__);                                     \
    } while (0)

#endif /* SERVICE_CORE_LISTEN_FAILED_H */
