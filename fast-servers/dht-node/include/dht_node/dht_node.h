/*
 * The boundary to the peer discovery module. Four calls, and dht-node/Architecture.md, *The
 * boundary*, is what they are transcribed from.
 *
 * Behind this header there will be a rust-libp2p node built as a static library; in front of it
 * there is C that must not know that. Nothing crosses but bytes and lengths: no Rust type, no
 * string the caller has to free, and no pointer into the module that outlives the call. A
 * drained record is a value copy, which is what lets the caller keep its arena discipline.
 *
 * Panics stop at the boundary and become an error code, for the same reason a C++ exception
 * must: unwinding into an event loop written in C is undefined behavior.
 *
 * Today the only implementation is src/dht_node_stub.c, which discovers nothing. The header is
 * here first on purpose -- the C side is written against the boundary, not against Rust, and
 * the day the crate lands nothing above this line changes.
 */
#ifndef DHT_NODE_H
#define DHT_NODE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dht_node dht_node;

/* Negative, so a drain can answer a count and an error through the same return value. */
typedef enum dht_status {
    DHT_OK = 0,
    DHT_ERR_INVALID_ARGUMENT = -1,
    DHT_ERR_NO_MEMORY = -2,
    DHT_ERR_NETWORK = -3,
    /* the module is present but does nothing: the stub, or a node without a topic */
    DHT_ERR_UNAVAILABLE = -4,
    /* a panic was caught at the boundary; the node is no longer usable */
    DHT_ERR_INTERNAL = -5
} dht_status;

typedef enum dht_peer_change {
    DHT_PEER_APPEARED = 1,
    DHT_PEER_CHANGED = 2,
    DHT_PEER_GONE = 3
} dht_peer_change;

/* Fixed size, so a drain is a memcpy into the caller's buffer and owns nothing afterwards. */
#define DHT_PEER_ID_MAX 64
#define DHT_ENDPOINT_MAX 256

typedef struct dht_peer_record {
    /* libp2p peer id, NUL-terminated. Derived from the community key pair -- and the derivation
     * is contracted, because the TypeScript node must arrive at the same id from the same seed.
     * See dht-node/Architecture.md, *The identity problem*. */
    char peer_id[DHT_PEER_ID_MAX];
    /* The community's API endpoint, not the multiaddr it was dialed on. */
    char endpoint[DHT_ENDPOINT_MAX];
    uint32_t api_version;
    /* Unix milliseconds, the same clock as a log line's `time`. */
    int64_t last_seen_ms;
    /* One of dht_peer_change. */
    uint8_t change;
} dht_peer_record;

typedef struct dht_node_config {
    /* 32 bytes. The key pair, and therefore the peer id, is derived from it. */
    const uint8_t *seed;
    size_t seed_len;
    /* The DHT key is derived from this string; both implementations must derive the same one. */
    const char *topic;
    /* Multiaddrs to listen on: TCP and QUIC for the same node are two entries, not one. */
    const char *const *listen_addrs;
    size_t listen_addr_count;
    /* Where to start. Empty is allowed -- see dht-node/Architecture.md, *Bootstrap*: a fresh
     * community asks a running one over plain HTTP and dials what it gets back. */
    const char *const *bootstrap_peers;
    size_t bootstrap_peer_count;
} dht_node_config;

/** Starts the node on its own runtime thread and hands back the handle. */
dht_status dht_node_init(const dht_node_config *config, dht_node **out);

/**
 * Copies out what changed since the last drain, at most @p capacity records. Returns the count,
 * or a negative dht_status.
 *
 * Never waits for the network: it answers with what has already arrived, or with zero. Changes
 * and not snapshots -- a caller that wants the full picture asks the database.
 */
int64_t dht_node_drain(dht_node *node, dht_peer_record *buffer, size_t capacity);

/** Publishes the local record now, out of band with the node's own timer. */
dht_status dht_node_announce(dht_node *node);

/** Stops the runtime and frees everything the handle owns. @p node is invalid afterwards. */
dht_status dht_node_shutdown(dht_node *node);

#ifdef __cplusplus
}
#endif

#endif /* DHT_NODE_H */
