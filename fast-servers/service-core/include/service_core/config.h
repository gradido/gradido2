/*
 * Startup configuration, read from the environment exactly once.
 *
 * Architecture.md, *Config*: env for what is needed at startup, a settings table for what is
 * dynamic, constants in code for what is fixed. This struct is the first of the three and
 * holds nothing that an admin could change while the process runs.
 *
 * Every string is a fixed-size buffer. A value that does not fit answers SC_ERR_TOO_LONG and
 * the process refuses to start -- it never truncates a host name or a topic into something
 * that would connect to the wrong place.
 */
#ifndef SERVICE_CORE_CONFIG_H
#define SERVICE_CORE_CONFIG_H

#include <stdint.h>

#include "service_core/log.h"
#include "service_core/status.h"

#define SC_CONFIG_HOST_MAX 64
#define SC_CONFIG_TOPIC_MAX 128
/* 32 bytes of seed, hex encoded, plus the terminator */
#define SC_CONFIG_SEED_HEX_MAX 65

typedef struct sc_config {
    /* Interface the HTTP roles bind to. Loopback by default: a fast server is expected to sit
     * behind a reverse proxy, and a default of 0.0.0.0 is how a development box ends up on
     * the public internet by accident. */
    char listen_host[SC_CONFIG_HOST_MAX];
    uint16_t backend_port;    /* BACKEND_PORT, legacy default 4000 */
    uint16_t federation_port; /* FEDERATION_PORT, legacy FEDERATION_MODULE_PORT 5010 */
    uint16_t dht_port;        /* DHT_PORT, legacy DHT_MODULE_PORT 5000 */

    /*
     * SERVER_THREADS: loops per HTTP role, one per core when unset.
     *
     * It is the number of cores and not more. Architecture.md, *Threading*, holds why: h2o is
     * thread-per-loop, so an oversubscribed thread does not delay one request but every
     * connection the kernel gave that loop. There is nothing to be gained by covering I/O wait
     * here, because waiting that has a file descriptor never occupies a thread in the first
     * place. The fallback backend serves on one whatever this says.
     */
    uint16_t server_threads;

    /* FEDERATION_DHT_TOPIC. Empty means peer discovery stays off, which is how legacy spells
     * it too -- see stage5.env, "if you set the value of FEDERATION_DHT_TOPIC". */
    char dht_topic[SC_CONFIG_TOPIC_MAX];
    /* FEDERATION_DHT_SEED, hex. The community key pair and therefore the libp2p peer id are
     * derived from it, so both implementations must read the same bytes out of it --
     * see dht-node/Architecture.md, *The identity problem*. */
    char dht_seed_hex[SC_CONFIG_SEED_HEX_MAX];

    sc_log_level log_level; /* LOG_LEVEL */
} sc_config;

/**
 * Fills @p out from the environment, applying the defaults above where a variable is unset.
 *
 * Answers SC_ERR_TOO_LONG for a value that would not fit and SC_ERR_MALFORMED for a port that
 * is not a number in 1..65535, in both cases having already logged which variable it was.
 */
sc_status sc_config_load(sc_config *out);

/** Logs the effective configuration at info, once, under `cat: "startup"`. No secrets: the
 *  seed is reported as present or absent and never printed. */
void sc_config_log(const sc_config *cfg);

#endif /* SERVICE_CORE_CONFIG_H */
