import { afterEach, describe, expect, test } from 'bun:test'
import { type BootstrapAnswer, registerPeerNetwork } from '@gradido/service-core'
import { ErrorCode } from '@gradido/shared/errors'
import { peerRoutes } from './peerRoutes'

/** peer.bootstrap answers from the node registered in this process, and says so when there is none. */

const answer: BootstrapAnswer = {
  self: { peerId: '12D3KooWexample', addresses: ['/ip4/192.0.2.1/tcp/5000'], delegation: '00' },
  peers: [],
}

afterEach(() => registerPeerNetwork(undefined))

describe('GET /peer/bootstrap', () => {
  test('is 503 PEER_NETWORK_UNAVAILABLE without a node in the process', async () => {
    const response = await peerRoutes().handle(new Request('http://localhost/peer/bootstrap'))
    expect(response.status).toBe(503)
    expect(await response.json()).toEqual({
      error: {
        code: ErrorCode.PeerNetworkUnavailable,
        name: 'PEER_NETWORK_UNAVAILABLE',
        message: 'this server runs no peer network node',
      },
    })
  })

  test('answers from the registered node, asking for DHT_BOOTSTRAP_PEERS_MAX peers', async () => {
    let asked = 0
    registerPeerNetwork({
      async bootstrapAnswer(maxPeers) {
        asked = maxPeers
        return answer
      },
    })
    const response = await peerRoutes().handle(new Request('http://localhost/peer/bootstrap'))
    expect(response.status).toBe(200)
    expect(await response.json()).toEqual(answer)
    expect(asked).toBe(20)
  })
})
