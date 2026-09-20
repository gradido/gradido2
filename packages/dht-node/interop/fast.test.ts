import { afterAll, beforeAll, describe, expect, test } from 'bun:test'
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, resolve } from 'node:path'
import { drive } from '../../../scripts/pty'
import { loadIdentity } from '../src/identity'
import { type DhtNode, startNode } from '../src/node'
import { community, eventually, recordingLogger } from '../src/testNodes'
import { providerKey, RPC_PROTOCOL } from '../src/wire'

/**
 * The reference node against the fast path's: `gradido2-fast`, the binary as it ships. Both ways
 * round -- the fast node joins through a peer.bootstrap the TypeScript node answers, and a
 * TypeScript node joins through the one a set-up fast instance serves beside its node -- and each
 * then finds the other's community by its key. Run by `bun run test:interop` at the root, which
 * builds the binary first; GRADIDO2_FAST names another one.
 */

const binary = resolve(
  process.env.GRADIDO2_FAST ?? join(import.meta.dir, '../../../fast-servers/zig-out/bin/gradido2-fast'),
)
const topic = 'interop-test'

interface CLine {
  readonly event?: string
  readonly data?: Record<string, unknown>
  readonly msg?: string
}

const freePort = () => 20000 + Math.floor(Math.random() * 20000)

let ts: DhtNode
let tsDelegation: Uint8Array
let cGroup: Uint8Array
let fast: ReturnType<typeof Bun.spawn> | undefined
let server: ReturnType<typeof Bun.serve>
let workdir: string
const cLog: CLine[] = []

async function readLines(stream: ReadableStream<Uint8Array>, into: CLine[]): Promise<void> {
  const decoder = new TextDecoder()
  let rest = ''
  for await (const chunk of stream) {
    rest += decoder.decode(chunk, { stream: true })
    let end = rest.indexOf('\n')
    while (end >= 0) {
      const line = rest.slice(0, end)
      rest = rest.slice(end + 1)
      try {
        into.push(JSON.parse(line) as CLine)
      } catch {
        into.push({ msg: line })
      }
      end = rest.indexOf('\n')
    }
  }
}

const logged = (event: string) => cLog.find((l) => l.event === event)

beforeAll(async () => {
  if (!existsSync(binary)) {
    throw new Error(`${binary} does not exist: run \`bun run zig build\` in fast-servers first`)
  }
  const own = await community(0xe1, 0x21)
  tsDelegation = own.delegation
  const tsIdentity = await loadIdentity(own.config)
  ts = await startNode({ topic, port: 0, public: true, identity: tsIdentity, logger: recordingLogger([]) })
  server = Bun.serve({
    port: 0,
    fetch: async (request) =>
      new URL(request.url).pathname === '/peer/bootstrap'
        ? Response.json(await ts.bootstrapAnswer(20))
        : new Response('not found', { status: 404 }),
  })

  const c = await community(0xe2, 0x22)
  cGroup = (await loadIdentity(c.config)).group
  /* An empty directory, so that no .env of a checkout configures the binary behind the test. */
  workdir = mkdtempSync(join(tmpdir(), 'gradido2-interop-'))
  fast = Bun.spawn([binary, '--dht-node'], {
    cwd: workdir,
    env: {
      PATH: process.env.PATH ?? '',
      DHT_TOPIC: topic,
      DHT_PORT: String(freePort()),
      MASTER_SEED: c.config.MASTER_SEED,
      DHT_DELEGATION: c.config.DHT_DELEGATION,
      DHT_REACHABILITY: 'public',
      DHT_BOOTSTRAP_URL: `http://127.0.0.1:${server.port}`,
      LOG_LEVEL: 'debug',
    },
    stdout: 'ignore',
    /* The fast path logs its JSON lines to stderr. */
    stderr: 'pipe',
  })
  void readLines(fast.stderr as ReadableStream<Uint8Array>, cLog)
  await eventually('the fast node', async () => logged('dht.node.started') ?? logged('dht.node.failed'))
  const failed = logged('dht.node.failed')
  if (failed !== undefined) {
    throw new Error(`the fast node did not start: ${failed.msg}`)
  }
}, 30_000)

afterAll(async () => {
  fast?.kill('SIGTERM')
  await fast?.exited
  server?.stop(true)
  await ts?.stop()
  if (workdir !== undefined) {
    rmSync(workdir, { recursive: true, force: true })
  }
}, 20_000)

describe('the fast path’s node', () => {
  test('joins through peer.bootstrap the reference node answers', async () => {
    const applied = await eventually('the bootstrap', async () =>
      logged('dht.bootstrap.applied') ?? logged('dht.bootstrap.failed'),
    )
    expect(applied.event).toBe('dht.bootstrap.applied')
    expect(applied.data?.peers).toBe(1)
  })

  test('is found by its community key from the reference node', async () => {
    const key = await providerKey(cGroup)
    const found = await eventually(
      'the fast community',
      async () => {
        for await (const event of ts.libp2p.services.dht.findProviders(key)) {
          if (event.name === 'PROVIDER' && event.providers.length > 0) {
            return event.providers[0]?.id.toString()
          }
        }
        return undefined
      },
      30_000,
    )
    expect(found).toBeDefined()
  }, 40_000)

  test('shows up in the reference node’s bootstrap answer', async () => {
    const answer = await eventually('the fast node among the peers', async () => {
      const current = await ts.bootstrapAnswer(20)
      return current.peers.length > 0 ? current : undefined
    })
    expect(answer.peers[0]?.peerId.startsWith('12D3KooW')).toBe(true)
  })

  test('drops a call naming an operation it does not take, at once', async () => {
    const peer = ts.libp2p.getConnections()[0]?.remotePeer
    if (peer === undefined) {
      throw new Error('the reference node has no connection to the fast one')
    }
    const started = Date.now()
    const stream = await ts.libp2p.dialProtocol(peer, RPC_PROTOCOL)
    const name = new TextEncoder().encode('/gradido/test/1')
    stream.send(
      new Uint8Array([1, ...tsDelegation, name.length, ...name, ...new TextEncoder().encode('ping')]),
    )
    await stream.close()
    const answer: Uint8Array[] = []
    try {
      for await (const chunk of stream) {
        answer.push(chunk instanceof Uint8Array ? chunk : chunk.subarray())
      }
    } catch {
      /* reset: the refusal */
    }
    expect(answer).toHaveLength(0)
    /* Dropped, not left to time out: the caller's failover moves on at once. */
    expect(Date.now() - started).toBeLessThan(2000)
    expect(logged('dht.call.refused')).toBeUndefined()
  })

  test('stops on SIGTERM and says so', async () => {
    fast?.kill('SIGTERM')
    await fast?.exited
    await eventually('the stop line', async () => logged('dht.node.stopped'))
    expect(logged('dht.node.stopped')?.data?.complete).toBe(true)
  })
})

describe('a set-up fast instance', () => {
  let instance: ReturnType<typeof Bun.spawn> | undefined
  let joiner: DhtNode | undefined
  let dir: string
  const log: CLine[] = []
  const backendPort = freePort()

  afterAll(async () => {
    instance?.kill('SIGTERM')
    await instance?.exited
    await joiner?.stop()
    rmSync(dir, { recursive: true, force: true })
  }, 20_000)

  test('answers peer.bootstrap from the node beside it, and a reference node joins through it', async () => {
    dir = mkdtempSync(join(tmpdir(), 'gradido2-interop-setup-'))
    const env = { PATH: process.env.PATH ?? '', HOME: process.env.HOME ?? '', TERM: 'xterm' }
    const setup = await drive([binary, 'setup'], {
      cwd: dir,
      env,
      answer: (screen) =>
        screen.includes('random keys')
          ? 'interop\r'
          : screen.includes('Which database?') && /❯ \S*postgresql/.test(screen)
            ? '\x1b[A\r'
            : undefined,
    })
    expect(setup.exitCode).toBe(0)
    const delegation = /^DHT_DELEGATION=([0-9a-f]+)$/m.exec(readFileSync(join(dir, '.env'), 'utf8'))?.[1]
    if (delegation === undefined) {
      throw new Error('setup wrote no DHT_DELEGATION')
    }
    const group = new Uint8Array(Buffer.from(delegation, 'hex').subarray(32, 64))

    /* The first community of its network: it joins through nobody. */
    instance = Bun.spawn([binary, '--backend', '--dht-node'], {
      cwd: dir,
      env: {
        ...env,
        DHT_TOPIC: topic,
        DHT_PORT: String(freePort()),
        BACKEND_PORT: String(backendPort),
        DHT_BOOTSTRAP_URL: '',
      },
      stdout: 'ignore',
      stderr: 'pipe',
    })
    void readLines(instance.stderr as ReadableStream<Uint8Array>, log)
    await eventually('the instance', async () =>
      log.some((l) => l.event === 'startup.server.started') &&
      log.some((l) => l.event === 'dht.node.started')
        ? true
        : undefined,
    )

    const answered = (await (
      await fetch(`http://127.0.0.1:${backendPort}/peer/bootstrap`)
    ).json()) as { self: { delegation: string; addresses: string[] } }
    expect(answered.self.delegation).toBe(delegation)
    expect(answered.self.addresses.length).toBeGreaterThan(0)

    const own = await community(0xe3, 0x23)
    const joinLog: { event: string; message: string }[] = []
    joiner = await startNode({
      topic,
      port: 0,
      public: true,
      identity: await loadIdentity(own.config),
      logger: recordingLogger(joinLog),
      bootstrapUrl: `http://127.0.0.1:${backendPort}`,
    })
    await eventually('the join', async () =>
      joinLog.find((l) => l.event === 'dht.bootstrap.applied' || l.event === 'dht.bootstrap.failed'),
    )
    expect(joinLog.some((l) => l.event === 'dht.bootstrap.applied')).toBe(true)

    const key = await providerKey(group)
    const node = joiner
    const found = await eventually(
      'the fast community',
      async () => {
        for await (const event of node.libp2p.services.dht.findProviders(key)) {
          if (event.name === 'PROVIDER' && event.providers.length > 0) {
            return true
          }
        }
        return undefined
      },
      30_000,
    )
    expect(found).toBe(true)
  }, 90_000)
})
