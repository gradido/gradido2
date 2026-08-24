/*
 * The stand-in for the rust-libp2p module: it accepts the configuration, reports no peers and
 * shuts down cleanly.
 *
 * It exists so the role can be written, started and shut down before the crate does, and so
 * `--dht-node` is a runnable mode rather than an immediate failure. It is not a fallback: a
 * process that needs peer discovery and gets this one silently finds nobody, which is why
 * dht_node_init logs a warning loud enough to notice.
 *
 * When the crate lands it replaces this file behind the same header, and the build gets an
 * option to choose between them. Nothing above dht_node.h changes.
 */
#include "dht_node/dht_node.h"

#include <stdlib.h>
#include <string.h>

#include "service_core/log.h"

struct dht_node {
    /* Kept so shutdown can say what it is stopping, and so the struct is not empty -- a zero
     * sized struct is a GNU extension and MSVC declines it. */
    char topic[128];
};

dht_status dht_node_init(const dht_node_config *config, dht_node **out)
{
    dht_node *node;
    size_t topic_len;

    if (config == NULL || out == NULL || config->topic == NULL)
        return DHT_ERR_INVALID_ARGUMENT;
    topic_len = strlen(config->topic);
    if (topic_len >= sizeof(node->topic))
        return DHT_ERR_INVALID_ARGUMENT;

    node = (dht_node *)calloc(1, sizeof(*node));
    if (node == NULL)
        return DHT_ERR_NO_MEMORY;
    memcpy(node->topic, config->topic, topic_len + 1);

    sc_log_warn(SC_CAT_FEDERATION, "dht.stub",
                "peer discovery is a stub: topic '%s' is announced to nobody and no peer is "
                "ever reported",
                node->topic);
    *out = node;
    return DHT_OK;
}

int64_t dht_node_drain(dht_node *node, dht_peer_record *buffer, size_t capacity)
{
    if (node == NULL || (buffer == NULL && capacity > 0))
        return DHT_ERR_INVALID_ARGUMENT;
    return 0;
}

dht_status dht_node_announce(dht_node *node)
{
    return node != NULL ? DHT_OK : DHT_ERR_INVALID_ARGUMENT;
}

dht_status dht_node_shutdown(dht_node *node)
{
    if (node == NULL)
        return DHT_ERR_INVALID_ARGUMENT;
    free(node);
    return DHT_OK;
}
