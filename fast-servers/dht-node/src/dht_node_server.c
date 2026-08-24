/*
 * The drain loop. The node decides when to announce; this decides when to look.
 */
#include "dht_node/dht_node_server.h"

#include <string.h>

#include "dht_node/dht_node.h"
#include "service_core/log.h"

/* dht-node/Architecture.md: the sweep runs every 20 seconds. */
#define DHT_DRAIN_INTERVAL_MS 20000
/* One drain's worth of records, on the stack. At 328 bytes each this is 21 KiB, which is a
 * thread stack's worth and not a heap allocation -- and a drain that fills it simply reports
 * the rest on the next pass, because the module keeps the state. */
#define DHT_DRAIN_BATCH 64

static const char *change_name(uint8_t change)
{
    switch (change) {
    case DHT_PEER_APPEARED:
        return "appeared";
    case DHT_PEER_CHANGED:
        return "changed";
    case DHT_PEER_GONE:
        return "gone";
    default:
        return "unknown";
    }
}

sc_status dht_node_server_run(const sc_config *cfg, const sc_quit_flag *quit)
{
    dht_node_config node_config;
    dht_node *node = NULL;
    dht_status status;
    unsigned int waited_ms = DHT_DRAIN_INTERVAL_MS; /* drain once immediately */

    if (cfg == NULL || quit == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    if (cfg->dht_topic[0] == '\0') {
        /* Legacy spells it the same way: no topic, no announcing and no listening. Asking for
         * the role without configuring it is a mistake worth a fatal rather than a process that
         * sits there looking healthy. */
        sc_log_fatal(SC_CAT_FEDERATION, "dht.topic_missing",
                     "--dht-node needs FEDERATION_DHT_TOPIC, which is unset");
        return SC_ERR_INVALID_ARGUMENT;
    }

    memset(&node_config, 0, sizeof(node_config));
    node_config.topic = cfg->dht_topic;
    /* The seed still has to be decoded from hex and the listen addresses still have to be built
     * from cfg->dht_port. Both belong here rather than behind the boundary -- the module is not
     * a config parser, see dht-node/Architecture.md, *What this module is not*. */

    status = dht_node_init(&node_config, &node);
    if (status != DHT_OK) {
        sc_log_fatal(SC_CAT_FEDERATION, "dht.init_failed", "peer discovery did not start: %d",
                     (int)status);
        return SC_ERR_NETWORK;
    }
    sc_log_info(SC_CAT_STARTUP, "dht.start", "dht-node is up on topic '%s', port %u",
                cfg->dht_topic, (unsigned)cfg->dht_port);

    while (!sc_quit_requested(quit)) {
        dht_peer_record batch[DHT_DRAIN_BATCH];
        int64_t count;
        int64_t i;

        if (waited_ms < DHT_DRAIN_INTERVAL_MS) {
            sc_runtime_sleep_ms(SC_RUNTIME_TICK_MS);
            waited_ms += SC_RUNTIME_TICK_MS;
            continue;
        }
        waited_ms = 0;

        count = dht_node_drain(node, batch, DHT_DRAIN_BATCH);
        if (count < 0) {
            sc_log_error(SC_CAT_FEDERATION, "dht.drain_failed", "drain answered %d", (int)count);
            continue;
        }
        for (i = 0; i < count; ++i) {
            /* Reported, not stored. The federation rows that follow are written by an
             * interaction through a repository, on whichever path is running. */
            sc_log_info(SC_CAT_FEDERATION, "dht.peer", "peer %s %s at %s, api %u", batch[i].peer_id,
                        change_name(batch[i].change), batch[i].endpoint,
                        (unsigned)batch[i].api_version);
        }
    }

    (void)dht_node_shutdown(node);
    sc_log_info(SC_CAT_STARTUP, "dht.stop", "dht-node stopped");
    return SC_OK;
}
