/*
 * peer.bootstrap from both ends: the answer this node gives the backend beside it, and joining a
 * network through another community's answer -- contracts/server/backend/peer.json.
 * packages/dht-node/src/bootstrap.ts and node.ts are the same on the reference path.
 */
#ifndef DHT_NODE_BOOTSTRAP_H
#define DHT_NODE_BOOTSTRAP_H

#include <stddef.h>
#include <stdint.h>

#include <uv.h>

#include "libp2p_ffi.h"
#include "service_core/runtime.h"
#include "service_core/status.h"

#define DHT_SELF_ADDRESSES_MAX 16
#define DHT_ADDRESS_MAX 256

/* What the answer says about this node. The addresses arrive as LP2P_EV_LISTENING on the role's
 * thread and are read on the backend's, hence the mutex. */
typedef struct dht_self {
    lp2p *node;
    uint8_t node_key[32];
    char delegation_hex[2 * 136 + 1];
    uv_mutex_t mutex;
    char addresses[DHT_SELF_ADDRESSES_MAX][DHT_ADDRESS_MAX];
    size_t address_count;
} dht_self;

sc_status dht_self_init(dht_self *self, lp2p *node, const uint8_t delegation[136]);
void dht_self_destroy(dht_self *self);
/** Records an address this node listens on; @p address need not be terminated. */
void dht_self_add_address(dht_self *self, const char *address, size_t len);

/** An sc_peer_network_answer_fn over a dht_self. */
sc_status dht_bootstrap_answer(void *self, uint32_t max_peers, char *out, size_t cap, size_t *len);

/* Room for fetching and parsing an answer, allocated once for the role. */
typedef struct dht_bootstrap_buffers {
    char *body;
    size_t body_bytes;
    uint8_t *arena;
    size_t arena_bytes;
} dht_bootstrap_buffers;

/**
 * Asks @p url for peer.bootstrap and hands what it names to the node's routing table, then
 * bootstraps the DHT from it. Logs dht.bootstrap.applied or dht.bootstrap.failed. Blocks for the
 * fetch -- ten seconds at most, and no longer than @p quit stays down.
 */
void dht_bootstrap_join(lp2p *node, const uint8_t own_key[32], const char *url,
                        const sc_quit_flag *quit, dht_bootstrap_buffers *buffers);

#endif /* DHT_NODE_BOOTSTRAP_H */
