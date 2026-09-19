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

#include "service_core/log/log.h"
#include "service_core/status.h"

#define SC_CONFIG_HOST_MAX 64
#define SC_CONFIG_TOPIC_MAX 128
/* DHT_DELEGATION: 136 bytes as hex, and the terminator. */
#define SC_CONFIG_DELEGATION_HEX_MAX 273
#define SC_CONFIG_URL_MAX 256

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

    /* DHT_TOPIC, legacy's FEDERATION_DHT_TOPIC. Empty means the network node stays off, as it did
     * there. It separates networks, so it is part of the node's protocol names. */
    char dht_topic[SC_CONFIG_TOPIC_MAX];
    /* DHT_DELEGATION, hex: the community key's signature over this instance's dht
     * node key, written by `setup` -- service_core/dht_delegation.h. Public, and the node key it
     * names is derived from MASTER_SEED, which is a secret and read where it is used rather than
     * held here. */
    char dht_delegation_hex[SC_CONFIG_DELEGATION_HEX_MAX];
    /* DHT_REACHABILITY: `public` or `private`, and private when unset. `setup` writes
     * public for a community URL on the public internet -- service_core/public_url.h -- and an
     * operator whose URL is public but whose dht port is not forwarded writes private. */
    int dht_public;
    /* DHT_BOOTSTRAP_URL: the community asked for peer.bootstrap at start, DHT_BOOTSTRAP_DEFAULT_URL
     * in contracts/const.json when unset. Empty joins through nobody -- the first community of a
     * network. */
    char dht_bootstrap_url[SC_CONFIG_URL_MAX];

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
