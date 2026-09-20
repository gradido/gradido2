/*
 * The dht-node role: the process mode that runs the network node.
 *
 * It is the caller in dht-node/Architecture.md's sense -- it fills the options, waits on the
 * node's events and hands them on; the node decides nothing. It writes no rows: legacy's
 * dht-node wrote federated_communities itself, and that is the one behavior deliberately not
 * carried over.
 */
#ifndef DHT_NODE_SERVER_H
#define DHT_NODE_SERVER_H

#include "service_core/config.h"
#include "service_core/runtime.h"
#include "service_core/status.h"

/** Runs until @p quit is raised. Same contract as backend_run. */
sc_status dht_node_server_run(const sc_config *cfg, const sc_quit_flag *quit);

#endif /* DHT_NODE_SERVER_H */
