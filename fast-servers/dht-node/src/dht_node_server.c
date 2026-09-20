/*
 * The event loop of the dht-node role. The node owns its threads; this one fills the options,
 * waits in lp2p_poll and hands on what arrives.
 */
#include "dht_node/dht_node_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootstrap.h"
#include "libp2p_ffi.h"
#include "service_core/dht_delegation.h"
#include "service_core/log/log.h"
#include "service_core/master_seed.h"
#include "service_core/peer_network.h"
#include "service_core/secret.h"

#define DHT_PROTOCOL_MAX 192
#define DHT_MULTIADDR_MAX 128
/* How often a node with no connection asks its bootstrap community again. */
#define DHT_REJOIN_INTERVAL_MS 60000u
#define DHT_BOOTSTRAP_BODY_BYTES (256u * 1024u)
#define DHT_BOOTSTRAP_ARENA_BYTES (512u * 1024u)

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Exactly 2 * len hex digits into len bytes, or 0. */
static int decode_hex(const char *hex, uint8_t *out, size_t len)
{
    size_t i;

    if (strlen(hex) != 2 * len)
        return 0;
    for (i = 0; i < len; ++i) {
        int hi = hex_nibble(hex[2 * i]);
        int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return 0;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 1;
}

/* An event of contracts/logging.json, category dht, with the data fields it contracts. */
#define DHT_LOG(level, event, values, ...)                                                          \
    do {                                                                                           \
        sc_log_context dht_log_ = {0};                                                             \
        dht_log_.data = (values);                                                                  \
        dht_log_.data_count = sizeof(values) / sizeof((values)[0]);                                \
        sc_log_event((level), SC_CAT_DHT, (event), &dht_log_, __VA_ARGS__);                        \
    } while (0)

/* dht.node.failed with one of its contracted reasons. The role ends after it. */
static void node_failed(const char *reason, const char *message)
{
    sc_log_value data[] = {SC_LOG_STR("reason", reason)};

    DHT_LOG(SC_LOG_FATAL, "dht.node.failed", data, "%s", message);
}

/* A limit's scope as contracts/logging.json spells it. */
static const char *scope_name(unsigned scope)
{
    switch (scope) {
    case LP2P_SCOPE_PEER:
        return "peer";
    case LP2P_SCOPE_IP_PREFIX:
        return "prefix";
    case LP2P_SCOPE_GLOBAL:
        return "global";
    case LP2P_LIMITED_BLOCKED:
        return "blocked";
    default:
        return "unknown";
    }
}

static void handle_event(lp2p *node, dht_self *self, const lp2p_event *event, const uint8_t *data)
{
    switch (event->type) {
    case LP2P_EV_LISTENING: {
        char address[256];
        const size_t length = event->data_len < sizeof(address) - 1 ? event->data_len
                                                                    : sizeof(address) - 1;
        sc_log_value values[1];

        memcpy(address, data, length);
        address[length] = '\0';
        dht_self_add_address(self, address, length);
        values[0] = (sc_log_value)SC_LOG_STR("address", address);
        DHT_LOG(SC_LOG_INFO, "dht.listener.started", values, "listening on %s", address);
        break;
    }
    case LP2P_EV_RPC_REQUEST: {
        /* Handing a call to the federation role -- in this process when it runs here, over its
         * contracted interface when it does not -- is not built yet. Refusing closes the stream
         * at once, which is kinder to the caller's failover than letting it time out. The role
         * registers no protocol names yet, so the operation is known by its index. */
        char protocol[12];
        sc_log_value values[1];

        (void)snprintf(protocol, sizeof(protocol), "%u", (unsigned)event->protocol);
        values[0] = (sc_log_value)SC_LOG_STR("protocol", protocol);
        DHT_LOG(SC_LOG_WARN, "dht.call.refused", values,
                "call on %s refused: no federation role to hand it to yet", protocol);
        (void)lp2p_rpc_reject(node, event->id);
        break;
    }
    case LP2P_EV_ANNOUNCEMENT: {
        /* Reported, not stored. Whether the community is new is decided by an interaction
         * through a repository, and every announcement is a hint until the handshake. */
        sc_log_value values[] = {SC_LOG_UINT("bytes", event->data_len)};
        DHT_LOG(SC_LOG_INFO, "dht.announcement.received", values, "announcement, %u bytes",
                (unsigned)event->data_len);
        break;
    }
    case LP2P_EV_LIMITED: {
        /* Debug, not warn: a refused flood is the limits working, and every line of it would be
         * the flood again, in the log. The module reports at most ten a second. */
        sc_log_value values[] = {SC_LOG_STR("reason", scope_name(event->reason))};
        DHT_LOG(SC_LOG_DEBUG, "dht.call.denied", values, "call denied: %s",
                scope_name(event->reason));
        break;
    }
    case LP2P_EV_OVERFLOW: {
        sc_log_value values[] = {SC_LOG_UINT("count", event->id)};
        DHT_LOG(SC_LOG_ERROR, "dht.event.dropped", values, "the node dropped %llu events",
                (unsigned long long)event->id);
        break;
    }
    default: {
        /* Unknown types are expected, not an error: the interface only grows. */
        sc_log_value values[] = {SC_LOG_UINT("type", event->type)};
        DHT_LOG(SC_LOG_DEBUG, "dht.event.ignored", values, "event type %u",
                (unsigned)event->type);
        break;
    }
    }
}

/*
 * The inbound RPC limits, per peer class and scope: requests a minute and the reserve --
 * DHT_CLASS_* and DHT_LIMIT_* in contracts/const.json, and packages/dht-node/src/limits.ts on the
 * other path. Which class a community is in is decided outside the node; until something hands
 * classes over, every community is DHT_CLASS_UNKNOWN. DHT_CLASS_OWN_INSTANCE has no limit.
 */
enum { DHT_CLASS_UNKNOWN = 0, DHT_CLASS_WITHOUT_URL = 1, DHT_CLASS_WITH_URL = 2 };

static const struct {
    uint8_t peer_class;
    uint8_t scope;
    uint32_t per_minute;
    uint32_t burst;
} kLimits[] = {
    {DHT_CLASS_UNKNOWN, LP2P_SCOPE_PEER, 6, 3},
    {DHT_CLASS_UNKNOWN, LP2P_SCOPE_IP_PREFIX, 30, 10},
    {DHT_CLASS_UNKNOWN, LP2P_SCOPE_GLOBAL, 300, 60},
    {DHT_CLASS_WITHOUT_URL, LP2P_SCOPE_PEER, 30, 10},
    {DHT_CLASS_WITHOUT_URL, LP2P_SCOPE_IP_PREFIX, 120, 30},
    {DHT_CLASS_WITHOUT_URL, LP2P_SCOPE_GLOBAL, 1200, 200},
    {DHT_CLASS_WITH_URL, LP2P_SCOPE_PEER, 120, 30},
    {DHT_CLASS_WITH_URL, LP2P_SCOPE_GLOBAL, 6000, 1000},
};

/*
 * The byte budget for published traffic -- the defaults of dht.mirror_bytes_per_second and
 * dht.mirror_bytes_burst in contracts/settings.json, until the settings table hands the configured
 * ones over. libp2p-ffi limits per class, so the budget applies to each class and they add up;
 * the settings say so.
 */
#define DHT_MIRROR_BYTES_PER_SECOND 65536u
#define DHT_MIRROR_BYTES_BURST 1048576u

static void apply_limits(lp2p *node)
{
    static const uint8_t kClasses[] = {DHT_CLASS_UNKNOWN, DHT_CLASS_WITHOUT_URL, DHT_CLASS_WITH_URL};
    const lp2p_rate budget = {DHT_MIRROR_BYTES_PER_SECOND, 1000u, DHT_MIRROR_BYTES_BURST};
    size_t i;

    for (i = 0; i != sizeof(kClasses); ++i) {
        const int32_t status = lp2p_limit_set_bytes(node, kClasses[i], LP2P_SCOPE_GLOBAL,
                                                    LP2P_PROTOCOL_TOPICS, budget);
        if (status != LP2P_OK) {
            sc_log_value values[] = {SC_LOG_UINT("peerClass", kClasses[i]),
                                     SC_LOG_STR("scope", "global")};
            DHT_LOG(SC_LOG_ERROR, "dht.limit.refused", values,
                    "the byte budget for class %u was refused: %d", (unsigned)kClasses[i],
                    (int)status);
        }
    }

    for (i = 0; i != sizeof(kLimits) / sizeof(kLimits[0]); ++i) {
        const lp2p_rate rate = {kLimits[i].per_minute, 60u * 1000u, kLimits[i].burst};
        const int32_t status = lp2p_limit_set(node, kLimits[i].peer_class, kLimits[i].scope,
                                              LP2P_PROTOCOL_ANY, rate);
        if (status != LP2P_OK) {
            sc_log_value values[] = {SC_LOG_UINT("peerClass", kLimits[i].peer_class),
                                     SC_LOG_STR("scope", scope_name(kLimits[i].scope))};
            DHT_LOG(SC_LOG_ERROR, "dht.limit.refused", values,
                    "the limit for class %u, scope %s was refused: %d",
                    (unsigned)kLimits[i].peer_class, scope_name(kLimits[i].scope), (int)status);
        }
    }
}

static void hex_of(const uint8_t *bytes, size_t size, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    for (i = 0; i != size; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 0xf];
    }
    out[2 * size] = '\0';
}

/* Clears a secret in a way the compiler may not drop as a dead store. */
static void wipe(void *p, size_t n)
{
    volatile uint8_t *bytes = (volatile uint8_t *)p;
    while (n-- != 0)
        *bytes++ = 0;
}

/*
 * The node's identity into @p options: its seed, derived from MASTER_SEED along `dht`, and the
 * delegation `setup` had the community key sign for it -- Architecture.md, *Communities and
 * instances*. The master seed is read here and wiped here; nothing that outlives the start holds
 * it. @p delegation is where options->delegation points afterwards.
 */
static sc_status load_identity(const sc_config *cfg, lp2p_options *options,
                               uint8_t delegation[SC_DHT_DELEGATION_BYTES])
{
    /* Longer than a seed, so that a value that is too long is reported as not being a seed. */
    char text[2 * SC_MASTER_SEED_HEX_SIZE];
    uint8_t seed[SC_MASTER_SEED_BYTES];
    uint8_t node_key[32];
    sc_status status;
    int parsed;

    status = sc_secret_read("MASTER_SEED", text, sizeof(text));
    if (status == SC_ERR_UNAVAILABLE)
        return status;
    parsed = status == SC_OK && sc_master_seed_parse(text, seed);
    if (!parsed) {
        const int empty = status == SC_OK && text[0] == '\0';
        wipe(text, sizeof(text));
        if (empty)
            node_failed("master-seed-missing", "--dht-node needs MASTER_SEED, which `setup` makes");
        else
            node_failed("master-seed-invalid", "--dht-node needs MASTER_SEED as 64 hex digits");
        return SC_ERR_INVALID_ARGUMENT;
    }
    wipe(text, sizeof(text));
    status = sc_master_seed_derive_dht(seed, options->node_seed, node_key);
    wipe(seed, sizeof(seed));
    if (status != SC_OK) {
        node_failed("identity", "the node identity could not be derived from MASTER_SEED");
        return status;
    }

    if (cfg->dht_delegation_hex[0] == '\0') {
        node_failed("delegation-missing", "--dht-node needs DHT_DELEGATION: run `setup`, which has "
                                          "the community key sign this instance's node key");
        return SC_ERR_INVALID_ARGUMENT;
    }
    if (!decode_hex(cfg->dht_delegation_hex, delegation, SC_DHT_DELEGATION_BYTES)) {
        node_failed("delegation-invalid", "DHT_DELEGATION is not 272 hex digits");
        return SC_ERR_INVALID_ARGUMENT;
    }
    /* The one way the two can drift apart in practice: a seed replaced after `setup` signed. */
    if (memcmp(delegation, node_key, sizeof(node_key)) != 0) {
        node_failed("delegation-foreign", "DHT_DELEGATION names another node than MASTER_SEED "
                                          "derives: run `setup` again to have this one signed");
        return SC_ERR_INVALID_ARGUMENT;
    }

    options->delegation = delegation;
    options->delegation_len = SC_DHT_DELEGATION_BYTES;
    memcpy(options->group, delegation + 32, sizeof(options->group));
    return SC_OK;
}

sc_status dht_node_server_run(const sc_config *cfg, const sc_quit_flag *quit)
{
    lp2p_options options;
    lp2p *node = NULL;
    char dht_protocol[DHT_PROTOCOL_MAX];
    char announce_topic[DHT_PROTOCOL_MAX];
    char listen_tcp[DHT_MULTIADDR_MAX];
    char listen_quic[DHT_MULTIADDR_MAX];
    const char *listen_addrs[2];
    uint8_t delegation[SC_DHT_DELEGATION_BYTES];
    char node_key_hex[65];
    uint8_t *buffer;
    size_t buffer_bytes;
    dht_self self;
    dht_bootstrap_buffers bootstrap = {NULL, DHT_BOOTSTRAP_BODY_BYTES, NULL,
                                       DHT_BOOTSTRAP_ARENA_BYTES};
    const int joins = cfg->dht_bootstrap_url[0] != '\0';
    uint64_t joined_ns = 0;
    sc_status identity;
    int32_t status;
    int failed = 0;

    if (cfg == NULL || quit == NULL)
        return SC_ERR_INVALID_ARGUMENT;

    if (cfg->dht_topic[0] == '\0') {
        /* No topic, no network, as in legacy. Asking for the role without configuring it is a
         * mistake worth a fatal rather than a process that sits there looking healthy. */
        node_failed("topic-missing", "--dht-node needs DHT_TOPIC, which is unset");
        return SC_ERR_INVALID_ARGUMENT;
    }

    lp2p_options_default(&options);

    /* contracts/const.json, DHT_RPC_* and DHT_RELAY_*. The relay limits only matter on a PUBLIC
     * node, the one that relays for others. */
    options.rpc_max_request_bytes = 1u << 20;
    options.rpc_max_response_bytes = 10u << 20;
    options.relay.max_reservations = 128;
    options.relay.reservation_duration_s = 60 * 60;
    options.relay.max_circuit_duration_s = 2 * 60;
    options.relay.max_circuit_bytes = 16u << 20;
    options.relay.max_reservations_per_peer = 4;
    options.relay.max_circuits = 16;
    options.relay.max_circuits_per_peer = 4;
    options.relay.circuits_per_peer = (lp2p_rate){1, 120u * 1000u, 30};
    options.relay.circuits_per_ip = (lp2p_rate){1, 60u * 1000u, 60};

    identity = load_identity(cfg, &options, delegation);
    if (identity != SC_OK)
        return identity;
    /* Configured, so AutoNAT never overrides it: PRIVATE reserves on relays and announces only
     * relayed addresses, PUBLIC announces its own and relays for others. */
    options.reachability = cfg->dht_public ? LP2P_REACH_PUBLIC : LP2P_REACH_PRIVATE;

    /* The topic separates networks, as it did in legacy -- a staging community must not find
     * production ones -- so it is part of every protocol name. The names themselves are
     * provisional until contracts/ holds them; both nodes must use the same ones. */
    (void)snprintf(dht_protocol, sizeof(dht_protocol), "/gradido/%s/kad/1", cfg->dht_topic);
    (void)snprintf(announce_topic, sizeof(announce_topic), "/gradido/%s/announce/1",
                   cfg->dht_topic);
    options.dht_protocol = dht_protocol;
    options.announce.topic = announce_topic;

    /* The node listens on every interface: unlike the HTTP roles it is not meant to sit behind
     * a reverse proxy, and a community without a URL is reachable through it or not at all. */
    (void)snprintf(listen_tcp, sizeof(listen_tcp), "/ip4/0.0.0.0/tcp/%u", (unsigned)cfg->dht_port);
    (void)snprintf(listen_quic, sizeof(listen_quic), "/ip4/0.0.0.0/udp/%u/quic-v1",
                   (unsigned)cfg->dht_port);
    listen_addrs[0] = listen_tcp;
    listen_addrs[1] = listen_quic;
    options.listen_addrs = listen_addrs;
    options.listen_addr_count = 2;

    /* A poll hands out whole records only, and one record carries a whole payload: the buffer
     * is sized for the largest one the node accepts. Allocated once, here, and never again. */
    buffer_bytes = (options.rpc_max_request_bytes > options.rpc_max_response_bytes
                        ? options.rpc_max_request_bytes
                        : options.rpc_max_response_bytes) +
                   sizeof(lp2p_event) + 8;
    buffer = (uint8_t *)malloc(buffer_bytes);
    if (joins) {
        bootstrap.body = (char *)malloc(bootstrap.body_bytes);
        bootstrap.arena = (uint8_t *)malloc(bootstrap.arena_bytes);
    }
    if (buffer == NULL || (joins && (bootstrap.body == NULL || bootstrap.arena == NULL))) {
        free(buffer);
        free(bootstrap.body);
        free(bootstrap.arena);
        node_failed("no-memory", "no memory for the event buffer");
        return SC_ERR_NO_MEMORY;
    }

    status = lp2p_start(&options, &node);
    wipe(options.node_seed, sizeof(options.node_seed));
    if (status != LP2P_OK || dht_self_init(&self, node, delegation) != SC_OK) {
        if (status == LP2P_OK)
            (void)lp2p_shutdown(node);
        free(buffer);
        free(bootstrap.body);
        free(bootstrap.arena);
        node_failed("network", "the network node did not start");
        return SC_ERR_NETWORK;
    }
    apply_limits(node);
    /* For the roles beside it in this process: the backend answers peer.bootstrap from it. */
    (void)sc_peer_network_register(dht_bootstrap_answer, &self);
    hex_of(delegation, 32, node_key_hex);
    {
        const char *reachability = cfg->dht_public ? "public" : "private";
        sc_log_value values[] = {SC_LOG_STR("nodeKey", node_key_hex),
                                 SC_LOG_UINT("port", cfg->dht_port),
                                 SC_LOG_STR("reachability", reachability)};
        DHT_LOG(SC_LOG_INFO, "dht.node.started", values, "dht-node %s is up on '%s', port %u, %s",
                node_key_hex, dht_protocol, (unsigned)cfg->dht_port, reachability);
    }

    if (joins) {
        dht_bootstrap_join(node, self.node_key, cfg->dht_bootstrap_url, quit, &bootstrap);
        joined_ns = uv_hrtime();
    }

    while (!sc_quit_requested(quit)) {
        int32_t written;
        size_t offset = 0;

        /* Joining again whenever the node finds itself alone -- a bootstrap community that was
         * down at start, or a network that dropped every connection. The fetch blocks this thread
         * for its duration; the module queues what arrives meanwhile. */
        if (joins && uv_hrtime() - joined_ns >= DHT_REJOIN_INTERVAL_MS * 1000000ull) {
            lp2p_stats stats;

            joined_ns = uv_hrtime();
            memset(&stats, 0, sizeof(stats));
            stats.size = (uint32_t)sizeof(stats);
            if (lp2p_stats_get(node, &stats) == LP2P_OK && stats.connections == 0)
                dht_bootstrap_join(node, self.node_key, cfg->dht_bootstrap_url, quit, &bootstrap);
        }

        /* A tick, not longer: the quit flag is only looked at between polls. */
        written = lp2p_poll(node, buffer, buffer_bytes, SC_RUNTIME_TICK_MS);
        if (written < 0) {
            /* The module is broken -- a panic, or it ended by itself -- and polling again would say
             * so once a tick. */
            node_failed("poll", "the network node stopped answering its poll");
            failed = 1;
            break;
        }
        while (offset + sizeof(lp2p_event) <= (size_t)written) {
            lp2p_event event;

            /* Copied out rather than cast: the buffer is bytes and carries no alignment. */
            memcpy(&event, buffer + offset, sizeof(event));
            if (event.size < sizeof(event) || event.size > (size_t)written - offset ||
                event.data_len > event.size - sizeof(event)) {
                sc_log_event(SC_LOG_ERROR, SC_CAT_DHT, "dht.event.refused", NULL,
                             "event record at %zu does not fit, dropping the rest", offset);
                break;
            }
            handle_event(node, &self, &event, buffer + offset + sizeof(event));
            offset += event.size;
        }
    }

    /* Before the node goes: clearing waits for an answer the backend is still reading. */
    (void)sc_peer_network_register(NULL, NULL);
    (void)lp2p_shutdown(node);
    dht_self_destroy(&self);
    free(buffer);
    free(bootstrap.body);
    free(bootstrap.arena);
    {
        sc_log_value values[] = {SC_LOG_BOOL("complete", 1)};
        DHT_LOG(SC_LOG_INFO, "dht.node.stopped", values, "dht-node stopped");
    }
    return failed ? SC_ERR_NETWORK : SC_OK;
}
