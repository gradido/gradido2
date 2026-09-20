/*
 * The stand-in for libp2p-ffi on a target the module publishes no prebuild for -- musl,
 * windows-gnu -- and under -Dlibp2p-ffi=stub: it accepts the options, reaches nobody, reports
 * nothing and shuts down cleanly. build.zig compiles it only when no module is linked.
 *
 * It is not a fallback. A process that needs the network and gets this one finds nobody, which
 * is why lp2p_start logs a warning loud enough to notice, and why a call answers
 * LP2P_ERR_UNAVAILABLE instead of pretending to have been sent.
 *
 * include/libp2p_ffi.h is a copy of the module's header (github.com/gradido/libp2p-ffi) at the
 * release build.zig.zon pins; keep it byte for byte.
 */
#include "libp2p_ffi.h"

#include <stdlib.h>
#include <string.h>

#include "service_core/log/log.h"
#include "service_core/runtime.h"

struct lp2p {
    /* Kept so shutdown can say what it is stopping, and so the struct is not empty -- a zero
     * sized struct is a GNU extension and MSVC declines it. */
    char dht_protocol[128];
};

uint32_t lp2p_abi_version(void)
{
    return LP2P_ABI_VERSION;
}

void lp2p_options_default(lp2p_options *opt)
{
    if (opt == NULL)
        return;
    memset(opt, 0, sizeof(*opt));
    opt->struct_size = (uint32_t)sizeof(*opt);
    /* rust-libp2p's request-response defaults: 1 MiB requests, 10 MiB responses, 10 s. */
    opt->rpc_max_request_bytes = 1u << 20;
    opt->rpc_max_response_bytes = 10u << 20;
    opt->rpc_timeout_ms = 10000;
    opt->quic = 1;
    opt->dcutr = 1;
    opt->autonat = 1;
    /* rust-libp2p's relay defaults -- dht-node/Architecture.md, *What libp2p provides*. */
    opt->relay.server = 1;
    opt->relay.client = 1;
    opt->relay.max_reservations = 128;
    opt->relay.max_reservations_per_peer = 4;
    opt->relay.reservation_duration_s = 60 * 60;
    opt->relay.max_circuits = 16;
    opt->relay.max_circuits_per_peer = 4;
    opt->relay.max_circuit_duration_s = 2 * 60;
    opt->relay.max_circuit_bytes = 1u << 17;
    opt->relay.circuits_per_peer = (lp2p_rate){1, 2 * 60 * 1000, 30};
    opt->relay.circuits_per_ip = (lp2p_rate){1, 60 * 1000, 60};
    opt->announce.enabled = 1;
    opt->announce.max_payload_bytes = 1024;
    opt->event_queue_bytes = 1u << 20;
    opt->topic_max_message_bytes = 64u << 10;
    opt->topic_max_subscriptions = 256;
}

int32_t lp2p_start(const lp2p_options *opt, lp2p **out)
{
    lp2p *node;
    size_t len;

    if (opt == NULL || out == NULL || opt->struct_size == 0 || opt->dht_protocol == NULL)
        return LP2P_ERR_INVALID_ARGUMENT;
    len = strlen(opt->dht_protocol);
    if (len >= sizeof(node->dht_protocol))
        return LP2P_ERR_INVALID_ARGUMENT;

    node = (lp2p *)calloc(1, sizeof(*node));
    if (node == NULL)
        return LP2P_ERR_NO_MEMORY;
    memcpy(node->dht_protocol, opt->dht_protocol, len + 1);

    sc_log_warn(SC_CAT_FEDERATION, "dht.stub",
                "the network node is a stub: '%s' is joined by nobody, no community is reachable "
                "and none is ever reported",
                node->dht_protocol);
    *out = node;
    return LP2P_OK;
}

int32_t lp2p_shutdown(lp2p *node)
{
    if (node == NULL)
        return LP2P_ERR_INVALID_ARGUMENT;
    free(node);
    return LP2P_OK;
}

int32_t lp2p_poll(lp2p *node, uint8_t *buf, size_t cap, int32_t timeout_ms)
{
    if (node == NULL || (buf == NULL && cap > 0) || timeout_ms < 0)
        return LP2P_ERR_INVALID_ARGUMENT;
    /* Nothing ever arrives, but a caller that waits here must still be made to wait -- a poll
     * that returned at once would turn the role thread into a busy loop. */
    if (timeout_ms > 0)
        sc_runtime_sleep_ms((unsigned int)timeout_ms);
    return 0;
}

int32_t lp2p_rpc_request(lp2p *node, const lp2p_key group, const lp2p_key node_key,
                         uint16_t protocol, const uint8_t *data, size_t len, uint32_t timeout_ms,
                         uint64_t *request_id)
{
    (void)node_key;
    (void)protocol;
    (void)timeout_ms;
    if (node == NULL || group == NULL || (data == NULL && len > 0) || request_id == NULL)
        return LP2P_ERR_INVALID_ARGUMENT;
    return LP2P_ERR_UNAVAILABLE;
}

int32_t lp2p_rpc_respond(lp2p *node, uint64_t request_id, const uint8_t *data, size_t len)
{
    (void)request_id;
    if (node == NULL || (data == NULL && len > 0))
        return LP2P_ERR_INVALID_ARGUMENT;
    /* No request was ever delivered, so there is none to answer. */
    return LP2P_ERR_INVALID_ARGUMENT;
}

int32_t lp2p_rpc_reject(lp2p *node, uint64_t request_id)
{
    (void)node;
    (void)request_id;
    /* The same: an unknown request id is an invalid argument, and every id is unknown here. */
    return LP2P_ERR_INVALID_ARGUMENT;
}

int32_t lp2p_peer_set_class(lp2p *node, const lp2p_key group, uint8_t peer_class)
{
    (void)peer_class;
    return node != NULL && group != NULL ? LP2P_OK : LP2P_ERR_INVALID_ARGUMENT;
}

int32_t lp2p_limit_set(lp2p *node, uint8_t peer_class, uint8_t scope, uint16_t protocol,
                       lp2p_rate rate)
{
    (void)peer_class;
    (void)protocol;
    (void)rate;
    if (node == NULL || scope > LP2P_SCOPE_GLOBAL)
        return LP2P_ERR_INVALID_ARGUMENT;
    return LP2P_OK;
}

int32_t lp2p_limit_set_bytes(lp2p *node, uint8_t peer_class, uint8_t scope, uint16_t protocol,
                             lp2p_rate rate)
{
    return lp2p_limit_set(node, peer_class, scope, protocol, rate);
}

int32_t lp2p_announce_set_payload(lp2p *node, const uint8_t *data, size_t len)
{
    return node != NULL && (data != NULL || len == 0) ? LP2P_OK : LP2P_ERR_INVALID_ARGUMENT;
}

int32_t lp2p_topic_subscribe(lp2p *node, const lp2p_key topic_key)
{
    /* Accepted and forgotten: a topic nobody else is on behaves exactly like this. */
    return node != NULL && topic_key != NULL ? LP2P_OK : LP2P_ERR_INVALID_ARGUMENT;
}

int32_t lp2p_topic_unsubscribe(lp2p *node, const lp2p_key topic_key)
{
    return node != NULL && topic_key != NULL ? LP2P_OK : LP2P_ERR_INVALID_ARGUMENT;
}

int32_t lp2p_topic_publish(lp2p *node, const lp2p_key topic_key, const uint8_t *data, size_t len)
{
    if (node == NULL || topic_key == NULL || (data == NULL && len > 0))
        return LP2P_ERR_INVALID_ARGUMENT;
    /* Nobody is on the topic, which the caller can see for itself: lp2p_topic_peers says 0. */
    return LP2P_OK;
}

int32_t lp2p_topic_peers(const lp2p *node, const lp2p_key topic_key)
{
    return node != NULL && topic_key != NULL ? 0 : LP2P_ERR_INVALID_ARGUMENT;
}

int32_t lp2p_add_address(lp2p *node, const lp2p_key node_key, const char *multiaddr)
{
    return node != NULL && node_key != NULL && multiaddr != NULL ? LP2P_OK
                                                                 : LP2P_ERR_INVALID_ARGUMENT;
}

int32_t lp2p_dht_bootstrap(lp2p *node, uint64_t *query_id)
{
    if (node == NULL || query_id == NULL)
        return LP2P_ERR_INVALID_ARGUMENT;
    return LP2P_ERR_UNAVAILABLE;
}

int32_t lp2p_dht_random_walk(lp2p *node, uint64_t *query_id)
{
    if (node == NULL || query_id == NULL)
        return LP2P_ERR_INVALID_ARGUMENT;
    return LP2P_ERR_UNAVAILABLE;
}

int32_t lp2p_dht_provide(lp2p *node, const lp2p_key record_key)
{
    if (node == NULL || record_key == NULL)
        return LP2P_ERR_INVALID_ARGUMENT;
    return LP2P_ERR_UNAVAILABLE;
}

int32_t lp2p_dht_stop_providing(lp2p *node, const lp2p_key record_key)
{
    if (node == NULL || record_key == NULL)
        return LP2P_ERR_INVALID_ARGUMENT;
    return LP2P_ERR_UNAVAILABLE;
}

int32_t lp2p_dht_find_providers(lp2p *node, const lp2p_key record_key, uint64_t *query_id)
{
    if (node == NULL || record_key == NULL || query_id == NULL)
        return LP2P_ERR_INVALID_ARGUMENT;
    return LP2P_ERR_UNAVAILABLE;
}

int32_t lp2p_routing_sample(lp2p *node, uint8_t *buf, size_t cap, uint32_t max_peers)
{
    (void)max_peers;
    if (node == NULL || (buf == NULL && cap > 0))
        return LP2P_ERR_INVALID_ARGUMENT;
    return 0;
}

int32_t lp2p_stats_get(const lp2p *node, lp2p_stats *out)
{
    uint32_t size;

    if (node == NULL || out == NULL || out->size < sizeof(uint32_t))
        return LP2P_ERR_INVALID_ARGUMENT;
    size = out->size < sizeof(*out) ? out->size : (uint32_t)sizeof(*out);
    /* Everything after the size field is zero: no connection, no peer, no call. */
    memset((uint8_t *)out + sizeof(uint32_t), 0, size - sizeof(uint32_t));
    return LP2P_OK;
}

int32_t lp2p_key_from_seed(const uint8_t seed[32], lp2p_key out)
{
    /* The stub has no ed25519 of its own; the module derives keys with rust-libp2p's. */
    return seed != NULL && out != NULL ? LP2P_ERR_UNAVAILABLE : LP2P_ERR_INVALID_ARGUMENT;
}

int32_t lp2p_delegation_sign(const uint8_t group_seed[32], const lp2p_key node_key,
                             uint64_t expires_ms, uint8_t out[LP2P_DELEGATION_BYTES])
{
    (void)expires_ms;
    return group_seed != NULL && node_key != NULL && out != NULL ? LP2P_ERR_UNAVAILABLE
                                                                 : LP2P_ERR_INVALID_ARGUMENT;
}

int32_t lp2p_delegation_verify(const uint8_t *delegation, size_t len, uint64_t now_ms,
                               lp2p_key group_out, lp2p_key node_out)
{
    (void)now_ms;
    (void)group_out;
    (void)node_out;
    return delegation != NULL && len == LP2P_DELEGATION_BYTES ? LP2P_ERR_UNAVAILABLE
                                                              : LP2P_ERR_INVALID_ARGUMENT;
}
