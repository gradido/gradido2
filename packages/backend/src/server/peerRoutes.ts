import { peerNetwork } from '@gradido/service-core'
import { ErrorCode, errorBody, errorStatus } from '@gradido/shared/errors'
import { Elysia } from 'elysia'

/** DHT_BOOTSTRAP_PEERS_MAX in contracts/const.json. */
const DHT_BOOTSTRAP_PEERS_MAX = 20

/**
 * The `peer` domain — `contracts/server/backend/peer.json`: how a fresh community gets into the
 * peer network.
 *
 * Answered from the dht-node role running in this process, which registered itself in
 * `@gradido/service-core`'s peer network registry; the backend never imports the node. A process
 * without one answers PEER_NETWORK_UNAVAILABLE, and the caller bootstraps from another community.
 * Public, because bootstrapping happens before any handshake exists.
 */
export const peerRoutes = () =>
  new Elysia({ name: 'gradido.peer', prefix: '/peer' }).get('/bootstrap', async ({ set }) => {
    const network = peerNetwork()
    if (network === undefined) {
      set.status = errorStatus(ErrorCode.PeerNetworkUnavailable)
      return errorBody(ErrorCode.PeerNetworkUnavailable)
    }
    return await network.bootstrapAnswer(DHT_BOOTSTRAP_PEERS_MAX)
  })

export type PeerRoutes = ReturnType<typeof peerRoutes>
