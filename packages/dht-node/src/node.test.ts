import { afterAll, beforeAll, describe, expect, test } from 'bun:test'
import { loadIdentity } from './identity'
import { type DhtNode, startNode } from './node'
import { community, eventually, type Logged, recordingLogger } from './testNodes'
import { providerKey, RPC_PROTOCOL } from './wire'

/**
 * Two nodes of the reference path on loopback, as two communities would meet: one finds the other
 * by its community key alone, calls it, and hears its announcements. What the node against
 * libp2p-ffi looks like is libp2p-ffi's own interop suite, interop/js.
 */

const topic = 'node-test'
const aLog: Logged[] = []
let a: DhtNode
let b: DhtNode
let aGroup: Uint8Array
let bDelegation: Uint8Array
let aDelegation: Uint8Array

beforeAll(async () => {
  const first = await community(0xa0, 0x07)
  const second = await community(0xb0, 0x08)
  const aIdentity = await loadIdentity(first.config)
  aGroup = aIdentity.group
  aDelegation = first.delegation
  bDelegation = second.delegation
  a = await startNode({
    topic,
    port: 0,
    public: true,
    identity: aIdentity,
    logger: recordingLogger(aLog),
    operations: ['/gradido/test/1'],
  })
  b = await startNode({
    topic,
    port: 0,
    public: true,
    identity: await loadIdentity(second.config),
    logger: recordingLogger([]),
  })
  const address = a.libp2p.getMultiaddrs().find((m) => m.toString().includes('/tcp/'))
  if (address === undefined) {
    throw new Error('the first node announces no TCP address')
  }
  await b.libp2p.dial(address)
})

afterAll(async () => {
  /* Together, which is the case js-libp2p does not finish by itself -- see STOP_TIMEOUT_MS. */
  await Promise.all([a?.stop(), b?.stop()])
}, 20_000)

/** A call from b to a naming @p operation: what a answered, which is nothing when it refused. */
async function call(operation: string): Promise<Uint8Array[]> {
  const stream = await b.libp2p.dialProtocol(a.libp2p.peerId, RPC_PROTOCOL)
  const name = new TextEncoder().encode(operation)
  stream.send(
    new Uint8Array([1, ...bDelegation, name.length, ...name, ...new TextEncoder().encode('ping')]),
  )
  await stream.close()
  const answer: Uint8Array[] = []
  try {
    for await (const chunk of stream) {
      answer.push(chunk instanceof Uint8Array ? chunk : chunk.subarray())
    }
  } catch {
    /* aborted: the refusal the caller's failover moves on from */
  }
  return answer
}

describe('the dht node', () => {
  test('is found by its community key, through the provider record', async () => {
    const key = await providerKey(aGroup)
    const found = await eventually('the provider record', async () => {
      for await (const event of b.libp2p.services.dht.findProviders(key)) {
        if (
          event.name === 'PROVIDER' &&
          event.providers.some((p) => p.id.equals(a.libp2p.peerId))
        ) {
          return true
        }
      }
      return undefined
    })
    expect(found).toBe(true)
  })

  test('drops a call naming an operation it does not take, without a word', async () => {
    expect(await call('/gradido/unknown/1')).toHaveLength(0)
    await Bun.sleep(200)
    expect(aLog.some((l) => l.event === 'dht.call.refused')).toBe(false)
  })

  test('refuses a call it has nobody to hand to, and says so', async () => {
    expect(await call('/gradido/test/1')).toHaveLength(0)
    await eventually('the rejection in the log', async () =>
      aLog.some((l) => l.event === 'dht.call.refused') ? true : undefined,
    )
  })

  test('reports a valid announcement and drops one with somebody else’s delegation', async () => {
    const announceTopic = `/gradido/${topic}/announce/1`
    await eventually('the announcement mesh', async () =>
      b.libp2p.services.pubsub.getSubscribers(announceTopic).length > 0 ? true : undefined,
    )
    await Bun.sleep(1500)
    const forged = new Uint8Array([1, ...aDelegation, ...new TextEncoder().encode('forged')])
    await b.libp2p.services.pubsub.publish(announceTopic, forged)
    const genuine = new Uint8Array([1, ...bDelegation, ...new TextEncoder().encode('api 1')])
    await b.libp2p.services.pubsub.publish(announceTopic, genuine)
    await eventually('the announcement in the log', async () =>
      aLog.some((l) => l.event === 'dht.announcement.received') ? true : undefined,
    )
    await Bun.sleep(500)
    const reported = aLog.filter((l) => l.event === 'dht.announcement.received')
    expect(reported.length).toBe(1)
    expect(reported[0]?.message).toBe('announcement, 5 bytes')
  })
})
