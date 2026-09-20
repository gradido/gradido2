import type { BootstrapAnswer, Logger } from '@gradido/service-core'
import { peerIdFromString } from '@libp2p/peer-id'
import { multiaddr } from '@multiformats/multiaddr'
import type { Libp2p } from 'libp2p'
import * as v from 'valibot'
import { verifyDelegation } from './wire'

/**
 * Joining the network: `peer.bootstrap` of one community, contracts/server/backend/peer.json.
 * `fast-servers/dht-node/src/bootstrap.c` is the other path.
 *
 * What the answer names is a hint, not a trust decision. `self` is checked -- its delegation has
 * to verify and to name the node the peer id says -- because an answer that cannot keep that
 * promise is not worth dialing anything of; the peers are only somewhere to start, and each is
 * verified when it is contacted, like everybody else.
 */

const hex = v.pipe(v.string(), v.regex(/^[0-9a-f]{272}$/u))
const answerSchema = v.object({
  self: v.object({ peerId: v.string(), addresses: v.array(v.string()), delegation: hex }),
  peers: v.array(
    v.object({ peerId: v.string(), addresses: v.array(v.string()), lastSeenAt: v.string() }),
  ),
})

/** The `reason` of dht.bootstrap.failed in contracts/logging.json. */
export type BootstrapFailure = 'unreachable' | 'invalid-answer' | 'delegation-invalid'

const FETCH_TIMEOUT_MS = 10_000
const DIAL_TIMEOUT_MS = 10_000

/** The answer at @p url, checked, or why there is none. */
export async function fetchBootstrap(url: string): Promise<BootstrapAnswer | BootstrapFailure> {
  let body: unknown
  try {
    const response = await fetch(`${url.replace(/\/+$/u, '')}/peer/bootstrap`, {
      signal: AbortSignal.timeout(FETCH_TIMEOUT_MS),
    })
    if (!response.ok) {
      return 'unreachable'
    }
    body = await response.json()
  } catch {
    return 'unreachable'
  }
  const parsed = v.safeParse(answerSchema, body)
  if (!parsed.success) {
    return 'invalid-answer'
  }
  const answer = parsed.output
  const delegation = await verifyDelegation(
    new Uint8Array(Buffer.from(answer.self.delegation, 'hex')),
  )
  let named: Uint8Array | undefined
  try {
    named = peerIdFromString(answer.self.peerId).publicKey?.raw
  } catch {
    return 'invalid-answer'
  }
  if (
    delegation === undefined ||
    named === undefined ||
    !Buffer.from(delegation.node).equals(Buffer.from(named))
  ) {
    return 'delegation-invalid'
  }
  return answer
}

/** Dials the answering node and the peers it named; answers how many of them took the call. */
export async function dialAnswer(libp2p: Libp2p, answer: BootstrapAnswer): Promise<number> {
  const own = libp2p.peerId.toString()
  let reached = 0
  for (const entry of [answer.self, ...answer.peers]) {
    if (entry.peerId === own) {
      continue
    }
    for (const address of entry.addresses) {
      const suffix = `/p2p/${entry.peerId}`
      try {
        const target = multiaddr(address.endsWith(suffix) ? address : `${address}${suffix}`)
        await libp2p.dial(target, { signal: AbortSignal.timeout(DIAL_TIMEOUT_MS) })
        reached += 1
        break
      } catch {
        /* the next address of the same node, or the next node */
      }
    }
  }
  return reached
}

/** One attempt to join through @p url, logged as dht.bootstrap.applied or dht.bootstrap.failed. */
export async function joinNetwork(libp2p: Libp2p, url: string, logger: Logger): Promise<void> {
  const answer = await fetchBootstrap(url)
  if (typeof answer === 'string') {
    logger.warn(
      { cat: 'dht', event: 'dht.bootstrap.failed', data: { url, reason: answer } },
      `bootstrap from ${url} failed: ${answer}`,
    )
    return
  }
  const peers = await dialAnswer(libp2p, answer)
  logger.info(
    { cat: 'dht', event: 'dht.bootstrap.applied', data: { url, peers } },
    `bootstrapped from ${url}: ${peers} nodes reached`,
  )
}
