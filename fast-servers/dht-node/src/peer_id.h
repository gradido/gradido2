/*
 * A node key as libp2p writes its peer id: base58btc over the identity multihash of the protobuf
 * public key -- 00 24 08 01 12 20 and the 32 key bytes, "12D3KooW..." in text. What js-libp2p's
 * peerIdFromString parses on the reference path; libp2p-ffi itself only ever takes the raw key.
 */
#ifndef DHT_NODE_PEER_ID_H
#define DHT_NODE_PEER_ID_H

#include <stddef.h>
#include <stdint.h>

/* An ed25519 peer id is 52 characters; the terminator and some room. */
#define DHT_PEER_ID_TEXT_MAX 64

/** @p key as a peer id, NUL terminated. */
void dht_peer_id_format(const uint8_t key[32], char out[DHT_PEER_ID_TEXT_MAX]);

/** The ed25519 key an ed25519 peer id names, or 0 for anything else. @p text need not be
 *  terminated. */
int dht_peer_id_parse(const char *text, size_t len, uint8_t key[32]);

#endif /* DHT_NODE_PEER_ID_H */
