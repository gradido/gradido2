/*
 * The peer network node of this process, for the roles beside it.
 *
 * The dht-node role registers an answer function here while it runs, and the backend answers
 * `peer.bootstrap` from it -- contracts/server/backend/peer.json. The backend never links the
 * node: a process without the role has nothing registered, and the route says so with
 * PEER_NETWORK_UNAVAILABLE. packages/service-core/src/peerNetwork is the same on the reference
 * path.
 */
#ifndef SERVICE_CORE_PEER_NETWORK_H
#define SERVICE_CORE_PEER_NETWORK_H

#include <stddef.h>
#include <stdint.h>

#include "service_core/status.h"

/**
 * Writes the `peer.bootstrap` answer as JSON into @p out, at most @p max_peers peers beside the
 * node itself, and its length into @p len. Answers SC_ERR_TOO_LONG when @p cap is too small.
 * Called on whichever thread serves the route.
 */
typedef sc_status (*sc_peer_network_answer_fn)(void *user_data, uint32_t max_peers, char *out,
                                               size_t cap, size_t *len);

/**
 * Registers the node's answer, or clears it with a NULL @p fn. Clearing waits for an answer in
 * progress, so after it returns @p user_data is no longer touched.
 */
sc_status sc_peer_network_register(sc_peer_network_answer_fn fn, void *user_data);

/** The registered node's answer, or SC_ERR_UNAVAILABLE when this process runs none. */
sc_status sc_peer_network_bootstrap_answer(uint32_t max_peers, char *out, size_t cap, size_t *len);

#endif /* SERVICE_CORE_PEER_NETWORK_H */
