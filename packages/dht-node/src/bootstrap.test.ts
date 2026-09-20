import { afterAll, beforeAll, describe, expect, test } from 'bun:test'
import { loadIdentity } from './identity'
import { type DhtNode, startNode } from './node'
import { community, eventually, type Logged, recordingLogger } from './testNodes'
import { providerKey } from './wire'

/**
 * Joining through peer.bootstrap. `a` answers the route the way the backend does, `b` already knows
 * `a`, and `c` knows nothing but the URL -- and then finds `b`'s community by its key.
 */

const topic = 'bootstrap-test'
let a: DhtNode
let b: DhtNode
let bGroup: Uint8Array
const nodes: DhtNode[] = []
let server: ReturnType<typeof Bun.serve>
/* What the route answers; a test swaps it for a forgery. */
let answer: (a: DhtNode) => Promise<unknown> = (node) => node.bootstrapAnswer(20)

async function joiner(seedByte: number, url: string, log: Logged[]): Promise<DhtNode> {
  const node = await startNode({
    topic,
    port: 0,
    public: true,
    identity: await loadIdentity((await community(0xd0 + seedByte, seedByte)).config),
    logger: recordingLogger(log),
    bootstrapUrl: url,
  })
  nodes.push(node)
  return node
}

beforeAll(async () => {
  a = await startNode({
    topic,
    port: 0,
    public: true,
    identity: await loadIdentity((await community(0xa1, 0x11)).config),
    logger: recordingLogger([]),
  })
  const bIdentity = await loadIdentity((await community(0xb1, 0x12)).config)
  bGroup = bIdentity.group
  b = await startNode({
    topic,
    port: 0,
    public: true,
    identity: bIdentity,
    logger: recordingLogger([]),
  })
  nodes.push(a, b)
  const address = a.libp2p.getMultiaddrs().find((m) => m.toString().includes('/tcp/'))
  if (address === undefined) {
    throw new Error('the first node announces no TCP address')
  }
  await b.libp2p.dial(address)
  server = Bun.serve({
    port: 0,
    fetch: async (request) =>
      new URL(request.url).pathname === '/peer/bootstrap'
        ? Response.json(await answer(a))
        : new Response('not found', { status: 404 }),
  })
})

afterAll(async () => {
  server?.stop(true)
  await Promise.all(nodes.map((n) => n.stop()))
}, 20_000)

describe('peer.bootstrap', () => {
  test('answers with the node itself and the peers it is connected to', async () => {
    const got = await eventually('b in the answer', async () => {
      const current = await a.bootstrapAnswer(20)
      return current.peers.some((p) => p.peerId === b.libp2p.peerId.toString())
        ? current
        : undefined
    })
    expect(got.self.peerId).toBe(a.libp2p.peerId.toString())
    expect(got.self.addresses.some((m) => m.includes('/p2p/'))).toBe(false)
    expect(got.self.delegation).toHaveLength(272)
  })

  test('names the addresses a PRIVATE node listens on, though it announces none', async () => {
    const node = await startNode({
      topic,
      port: 0,
      public: false,
      identity: await loadIdentity((await community(0xc1, 0x16)).config),
      logger: recordingLogger([]),
    })
    nodes.push(node)
    const got = await eventually('the listen addresses', async () => {
      const current = await node.bootstrapAnswer(20)
      return current.self.addresses.length > 0 ? current : undefined
    })
    expect(got.self.addresses.some((m) => m.includes('/tcp/'))).toBe(true)
  })

  test('lets a node that knows only the URL join and find a community by key', async () => {
    const log: Logged[] = []
    const c = await joiner(0x13, `http://127.0.0.1:${server.port}`, log)
    await eventually('the bootstrap', async () =>
      log.some((l) => l.event === 'dht.bootstrap.applied') ? true : undefined,
    )
    const key = await providerKey(bGroup)
    const found = await eventually('b by its community key', async () => {
      for await (const event of c.libp2p.services.dht.findProviders(key)) {
        if (
          event.name === 'PROVIDER' &&
          event.providers.some((p) => p.id.equals(b.libp2p.peerId))
        ) {
          return true
        }
      }
      return undefined
    })
    expect(found).toBe(true)
  })

  test('drops an answer whose delegation does not name its node', async () => {
    answer = async (node) => {
      const genuine = await node.bootstrapAnswer(20)
      return { ...genuine, self: { ...genuine.self, peerId: b.libp2p.peerId.toString() } }
    }
    const log: Logged[] = []
    const d = await joiner(0x14, `http://127.0.0.1:${server.port}`, log)
    const failed = await eventually('the refusal', async () =>
      log.find((l) => l.event === 'dht.bootstrap.failed'),
    )
    expect(failed.message).toContain('delegation-invalid')
    expect(d.libp2p.getConnections()).toHaveLength(0)
  })

  test('reports a URL nobody answers', async () => {
    const log: Logged[] = []
    await joiner(0x15, 'http://127.0.0.1:1', log)
    const failed = await eventually('the failure', async () =>
      log.find((l) => l.event === 'dht.bootstrap.failed'),
    )
    expect(failed.message).toContain('unreachable')
  })
})
