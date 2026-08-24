/*
 * The environment, read once at startup. Defaults follow legacy's variable names and ports
 * (stage5.env), so an operator moving a community from legacy to gradido2 does not have to
 * relearn them.
 */
#include "service_core/config.h"

#include <stdlib.h>
#include <string.h>

#include "service_core/log.h"

#define DEFAULT_LISTEN_HOST "127.0.0.1"
#define DEFAULT_BACKEND_PORT 4000
#define DEFAULT_FEDERATION_PORT 5010
#define DEFAULT_DHT_PORT 5000

/**
 * Copies the environment variable @p name into @p dst, or @p fallback when it is unset.
 *
 * A value that does not fit answers SC_ERR_TOO_LONG. It never truncates: half a host name is a
 * host name, and it is the wrong one.
 */
static sc_status copy_env(char *dst, size_t dst_size, const char *name, const char *fallback)
{
    const char *value = getenv(name);
    size_t len;

    if (value == NULL)
        value = fallback;
    len = strlen(value);
    if (len >= dst_size) {
        sc_log_fatal(SC_CAT_STARTUP, "config.value_too_long", "%s is %zu bytes, the limit is %zu",
                     name, len, dst_size - 1);
        return SC_ERR_TOO_LONG;
    }
    memcpy(dst, value, len + 1);
    return SC_OK;
}

static sc_status read_port(uint16_t *out, const char *name, uint16_t fallback)
{
    const char *value = getenv(name);
    char *end;
    unsigned long parsed;

    if (value == NULL || value[0] == '\0') {
        *out = fallback;
        return SC_OK;
    }
    parsed = strtoul(value, &end, 10);
    if (*end != '\0' || parsed == 0 || parsed > 65535) {
        sc_log_fatal(SC_CAT_STARTUP, "config.port_invalid",
                     "%s is '%s', which is not a port between 1 and 65535", name, value);
        return SC_ERR_MALFORMED;
    }
    *out = (uint16_t)parsed;
    return SC_OK;
}

sc_status sc_config_load(sc_config *out)
{
    sc_status status;

    if (out == NULL)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    out->log_level = sc_log_level_from_name(getenv("LOG_LEVEL"), SC_LOG_INFO);

    status =
        copy_env(out->listen_host, sizeof(out->listen_host), "LISTEN_HOST", DEFAULT_LISTEN_HOST);
    if (status != SC_OK)
        return status;
    status = copy_env(out->dht_topic, sizeof(out->dht_topic), "FEDERATION_DHT_TOPIC", "");
    if (status != SC_OK)
        return status;
    status = copy_env(out->dht_seed_hex, sizeof(out->dht_seed_hex), "FEDERATION_DHT_SEED", "");
    if (status != SC_OK)
        return status;

    status = read_port(&out->backend_port, "BACKEND_PORT", DEFAULT_BACKEND_PORT);
    if (status != SC_OK)
        return status;
    status = read_port(&out->federation_port, "FEDERATION_PORT", DEFAULT_FEDERATION_PORT);
    if (status != SC_OK)
        return status;
    status = read_port(&out->dht_port, "DHT_PORT", DEFAULT_DHT_PORT);
    if (status != SC_OK)
        return status;

    return SC_OK;
}

void sc_config_log(const sc_config *cfg)
{
    if (cfg == NULL)
        return;
    /* The seed is reported as present or absent. Printing it would put the community's private
     * key into every log aggregator the operator happens to run. */
    sc_log_info(SC_CAT_STARTUP, "config.loaded",
                "host %s, backend %u, federation %u, dht %u, topic %s, seed %s, log level %d",
                cfg->listen_host, (unsigned)cfg->backend_port, (unsigned)cfg->federation_port,
                (unsigned)cfg->dht_port, cfg->dht_topic[0] != '\0' ? cfg->dht_topic : "(unset)",
                cfg->dht_seed_hex[0] != '\0' ? "set" : "(unset)", (int)cfg->log_level);
}
