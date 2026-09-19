/**
 * The peer network node of this process, for the roles beside it.
 *
 * The dht-node role registers itself here when it starts in a process that runs other roles too,
 * and the backend answers `peer.bootstrap` from it -- contracts/server/backend/peer.json. The
 * backend never imports the node: a process without one simply has nothing registered, and the
 * route says so. `fast-servers/service-core/include/service_core/peer_network.h` is the same on
 * the fast path.
 */

/** What `peer.bootstrap` answers, as the contract spells it. */
export interface BootstrapAnswer {
  readonly self: {
    readonly peerId: string
    readonly addresses: readonly string[]
    /** DHT_DELEGATION: 136 bytes, lowercase hex. */
    readonly delegation: string
  }
  readonly peers: readonly {
    readonly peerId: string
    readonly addresses: readonly string[]
    /** Unix milliseconds as a decimal string -- contracts/types/Timestamp.json. */
    readonly lastSeenAt: string
  }[]
}

export interface PeerNetwork {
  /** At most @p maxPeers peers beside `self`. */
  bootstrapAnswer(maxPeers: number): Promise<BootstrapAnswer>
}

let registered: PeerNetwork | undefined

/** Registers this process's node, or clears it with undefined when the node stops. */
export function registerPeerNetwork(network: PeerNetwork | undefined): void {
  registered = network
}

/** The node running in this process, or undefined when there is none. */
export function peerNetwork(): PeerNetwork | undefined {
  return registered
}
