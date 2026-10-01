/*
 * The one line a configuration that cannot be used is reported with:
 * `contracts/logging.json`, `startup.config.failed`.
 *
 * A macro and not a function, so that the sentence stays a printf call the compiler checks
 * where it is written. @p variable is the environment variable at fault; @p reason is one of
 * the contract's closed vocabulary -- "invalid", "unreadable", "too-long". The sentence may
 * quote the value it refused, and must not when the variable is a secret.
 *
 * `packages/service-core/src/config/grabEnvAndCheckSchema.ts` writes the same line on the
 * reference path.
 */
#ifndef SERVICE_CORE_CONFIG_FAILED_H
#define SERVICE_CORE_CONFIG_FAILED_H

#include "service_core/log/log.h"

#define SC_CONFIG_FAILED(variable, reason, ...)                                                   \
    do {                                                                                          \
        sc_log_value sc_config_failed_data[2] = {SC_LOG_STR("variable", (variable)),              \
                                                 SC_LOG_STR("reason", (reason))};                 \
        sc_log_context sc_config_failed_context = {0};                                            \
                                                                                                  \
        sc_config_failed_context.data = sc_config_failed_data;                                    \
        sc_config_failed_context.data_count = 2;                                                  \
        sc_log_event(SC_LOG_FATAL, SC_CAT_STARTUP, "startup.config.failed",                       \
                     &sc_config_failed_context, __VA_ARGS__);                                     \
    } while (0)

#endif /* SERVICE_CORE_CONFIG_FAILED_H */
