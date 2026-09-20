import { noise } from '@chainsafe/libp2p-noise'
import { quic } from '@chainsafe/libp2p-quic'
import { yamux } from '@chainsafe/libp2p-yamux'
import type { BootstrapAnswer, Logger, PeerNetwork } from '@gradido/service-core'
import { autoNAT } from '@libp2p/autonat'
import { circuitRelayServer, circuitRelayTransport } from '@libp2p/circuit-relay-v2'
import { dcutr } from '@libp2p/dcutr'
import { type GossipSub, gossipsub, TopicValidatorResult } from '@libp2p/gossipsub'
import { identify } from '@libp2p/identify'
import type { Connection, Stream } from '@libp2p/interface'
import { type KadDHT, kadDHT, passthroughMapper } from '@libp2p/kad-dht'
import { ping } from '@libp2p/ping'
import { tcp } from '@libp2p/tcp'
import type { Multiaddr } from '@multiformats/multiaddr'
import { createLibp2p, type Libp2p } from 'libp2p'
import { joinNetwork } from './bootstrap'
import type { Identity } from './identity'
import { AnnouncementRate, Limits } from './limits'
import { parseRequest, parseResponse, providerKey, RPC_PROTOCOL, verifyDelegation } from './wire'

/**
 * The dht-node role's node: js-libp2p configured to be the same node libp2p-ffi is on the fast
 * path -- `fast-servers/dht-node/src/dht_node_server.c`. What it takes from the configuration,
 * which events it logs and what it refuses are the same; the protocol names are contracts/const.json's.
 *
 * What js-libp2p does not do by itself, and libp2p-ffi does, is here: the delegation check on
 * every call and announcement, the limits per class, and the provider record under the group key.
 */

/** DHT_RPC_MAX_REQUEST_BYTES, plus the frame around a payload. */
const MAX_REQUEST_BYTES = (1 << 20) + 2 + 136 + 255
/**
 * DHT_RELAY_* in contracts/const.json, what js-libp2p's relay can be told. The per-peer and per-IP
 * limits the fast path also applies have no counterpart here.
 */
const RELAY = {
  maxReservations: 128,
  reservationTtl: 3600 * 1000,
  defaultDurationLimit: 120 * 1000,
  defaultDataLimit: BigInt(16 << 20),
}
/** DHT_ANNOUNCE_MAX_PAYLOAD_BYTES in contracts/const.json. */
const MAX_ANNOUNCEMENT_PAYLOAD_BYTES = 1024
/** How long a burst of new peers waits before the provider record is published once. */
const PROVIDE_DEBOUNCE_MS = 2000
/**
 * How long stopping may take. js-libp2p's stop() does not return when the peer at the other end of
 * a connection closes it at the same moment -- two servers restarted together -- so the role does
 * not wait for it past this; the process exit closes what is left.
 */
const STOP_TIMEOUT_MS = 5000
/** How often a node with no connection asks its bootstrap community again. */
const REJOIN_INTERVAL_MS = 60_000

export interface NodeOptions {
  readonly topic: string
  readonly port: number
  readonly public: boolean
  readonly identity: Identity
  readonly logger: Logger
  /** DHT_BOOTSTRAP_URL: the community asked for peers at start, or undefined for none. */
  readonly bootstrapUrl?: string
  /** The operation names a call may carry; a call naming another is dropped without an event, as
   *  libp2p-ffi's rpc_protocols does. None until a federation role takes calls. */
  readonly operations?: readonly string[]
}

export interface DhtNode extends PeerNetwork {
  readonly libp2p: Libp2p<{ dht: KadDHT; pubsub: GossipSub }>
  readonly limits: Limits
  /** Answers whether the node stopped completely, or was left to the process exit. */
  stop(): Promise<boolean>
}

export async function startNode(options: NodeOptions): Promise<DhtNode> {
  const { logger, identity } = options
  const kadProtocol = `/gradido/${options.topic}/kad/1`
  const announceTopic = `/gradido/${options.topic}/announce/1`
  const limits = new Limits()
  const announcements = new AnnouncementRate()
  const operations = options.operations ?? []

  const services = {
    identify: identify(),
    // js-libp2p's DHT pings a peer before it keeps it; libp2p-ffi answers ping for the same reason.
    ping: ping(),
    /* Server mode on both kinds of node: a private one answers queries through its relays. And
       every address is kept, as libp2p-ffi keeps them -- the default mapper would drop private
       ones, which is right for the public IPFS network and wrong for a LAN of communities. */
    dht: kadDHT({ protocol: kadProtocol, clientMode: false, peerInfoMapper: passthroughMapper }),
    pubsub: gossipsub({ allowPublishToZeroTopicPeers: true }),
    autonat: autoNAT(),
    dcutr: dcutr(),
    /* Only a node others can dial relays for them. */
    ...(options.public ? { relay: circuitRelayServer({ reservations: RELAY }) } : {}),
  }

  const listen = [`/ip4/0.0.0.0/tcp/${options.port}`, `/ip4/0.0.0.0/udp/${options.port}/quic-v1`]
  const libp2p = await createLibp2p({
    privateKey: identity.privateKey,
    addresses: {
      /* A private node reserves on relays and is reached through them; it announces nothing
         else, because nothing else leads back to it. */
      listen: options.public ? listen : [...listen, '/p2p-circuit'],
      ...(options.public
        ? {}
        : { announceFilter: (addrs: Multiaddr[]) => addrs.filter(isRelayed) }),
    },
    transports: [tcp(), quic(), circuitRelayTransport()],
    connectionEncrypters: [noise()],
    streamMuxers: [yamux()],
    services,
    /* Started below, once the listeners are attached that report what it listens on. */
    start: false,
  })

  await libp2p.handle(
    RPC_PROTOCOL,
    async (stream: Stream, connection: Connection) => {
      await answer(stream, connection, operations, limits, logger)
    },
    /* A community without a URL is reached over a relay, and js-libp2p refuses streams on relayed
       connections unless the handler says otherwise. */
    { runOnLimitedConnection: true },
  )

  const pubsub = libp2p.services.pubsub
  pubsub.topicValidators.set(announceTopic, async (peer, message) => {
    if (message.type !== 'signed') {
      return TopicValidatorResult.Reject
    }
    const frame = parseResponse(message.data)
    if (frame === undefined || frame.payload.length > MAX_ANNOUNCEMENT_PAYLOAD_BYTES) {
      return TopicValidatorResult.Reject
    }
    const delegation = await verifyDelegation(frame.delegation)
    const source = message.from.publicKey?.raw
    if (delegation === undefined || source === undefined || !sameBytes(delegation.node, source)) {
      return TopicValidatorResult.Reject
    }
    if (
      limits.admit({ group: delegation.group, peer: peer.toString(), ip: undefined }) === 'blocked'
    ) {
      return TopicValidatorResult.Reject
    }
    /* Not the sender's fault as far as this node can tell, so not a rejection -- just not passed on. */
    return announcements.allow(message.from.toString())
      ? TopicValidatorResult.Accept
      : TopicValidatorResult.Ignore
  })
  pubsub.addEventListener('message', (event: CustomEvent) => {
    const message = event.detail
    if (message.topic === announceTopic) {
      /* Reported, not stored: every announcement is a hint until the handshake. */
      const bytes = message.data.length - 137
      logger.info(
        { cat: 'dht', event: 'dht.announcement.received', data: { bytes } },
        `announcement, ${bytes} bytes`,
      )
    }
  })

  /* The provider record under the group key is what makes this community findable. It is
     published once the node has somebody to publish it to, again when new peers arrive, and
     republished by the DHT on its own schedule afterwards. */
  const groupKey = await providerKey(identity.group)
  let provideTimer: ReturnType<typeof setTimeout> | undefined
  const provideSoon = () => {
    if (provideTimer !== undefined) {
      return
    }
    provideTimer = setTimeout(async () => {
      provideTimer = undefined
      try {
        for await (const _ of libp2p.services.dht.provide(groupKey)) {
          /* drained: provide() does its work while it is iterated */
        }
      } catch (error) {
        logger.debug(
          { cat: 'dht', event: 'dht.provider.failed' },
          `provide failed: ${String(error)}`,
        )
      }
    }, PROVIDE_DEBOUNCE_MS)
  }
  libp2p.addEventListener('peer:connect', provideSoon)

  /* What the node listens on, the way the C path reports it: the transports' own addresses, and a
     relayed one once a relay has taken a reservation. What it announces is a subset of these. */
  const reported = new Set<string>()
  const report = (addresses: Multiaddr[]) => {
    for (const address of addresses) {
      const text = address.toString()
      if (!reported.has(text)) {
        reported.add(text)
        logger.info(
          { cat: 'dht', event: 'dht.listener.started', data: { address: text } },
          `listening on ${text}`,
        )
      }
    }
  }
  libp2p.addEventListener('transport:listening', (event) => report(event.detail.getAddrs()))
  libp2p.addEventListener('self:peer:update', () =>
    report(libp2p.getMultiaddrs().filter(isRelayed)),
  )
  await libp2p.start()
  pubsub.subscribe(announceTopic)

  /* Joining: once now, and again whenever the node finds itself alone -- a bootstrap community
     that was down at start, or a network that dropped every connection. */
  let rejoin: ReturnType<typeof setInterval> | undefined
  if (options.bootstrapUrl !== undefined) {
    const url = options.bootstrapUrl
    /* joinNetwork logs its own failures; a rejection is a dial after stop, and nothing to report. */
    const join = () => joinNetwork(libp2p, url, logger).catch(() => undefined)
    join()
    rejoin = setInterval(() => {
      if (libp2p.getConnections().length === 0) {
        join()
      }
    }, REJOIN_INTERVAL_MS)
  }

  let sampleOffset = 0
  const delegationHex = Buffer.from(identity.delegation).toString('hex')
  const ownPeerId = libp2p.peerId.toString()

  return {
    libp2p,
    limits,
    /* peer.bootstrap: this node, and a sample of the DHT peers it is connected to now -- a
       different slice on every call, so that joining nodes fan out from different places. */
    async bootstrapAnswer(maxPeers: number): Promise<BootstrapAnswer> {
      const now = String(Date.now())
      const candidates: BootstrapAnswer['peers'][number][] = []
      const seen = new Set<string>()
      for (const connection of libp2p.getConnections()) {
        const id = connection.remotePeer.toString()
        if (seen.has(id)) {
          continue
        }
        seen.add(id)
        const peer = await libp2p.peerStore.get(connection.remotePeer).catch(() => undefined)
        if (peer === undefined || !peer.protocols.includes(kadProtocol)) {
          continue
        }
        const addresses = peer.addresses.map((a) => a.multiaddr.toString())
        if (addresses.length !== 0) {
          candidates.push({ peerId: id, addresses, lastSeenAt: now })
        }
      }
      sampleOffset = candidates.length === 0 ? 0 : (sampleOffset + 1) % candidates.length
      const peers = [...candidates.slice(sampleOffset), ...candidates.slice(0, sampleOffset)].slice(
        0,
        maxPeers,
      )
      const suffix = `/p2p/${ownPeerId}`
      return {
        self: {
          peerId: ownPeerId,
          /* What it listens on, as the C node answers -- not what it announces, which is nothing
             on a PRIVATE node before a relay took it. */
          addresses: [...reported].map((a) =>
            a.endsWith(suffix) ? a.slice(0, -suffix.length) : a,
          ),
          delegation: delegationHex,
        },
        peers,
      }
    },
    async stop() {
      if (provideTimer !== undefined) {
        clearTimeout(provideTimer)
      }
      if (rejoin !== undefined) {
        clearInterval(rejoin)
      }
      let timer: ReturnType<typeof setTimeout> | undefined
      const stopped = await Promise.race([
        Promise.resolve(libp2p.stop()).then(() => true),
        new Promise<false>((resolve) => {
          timer = setTimeout(() => resolve(false), STOP_TIMEOUT_MS)
        }),
      ])
      clearTimeout(timer)
      return stopped
    },
  }
}

/**
 * An inbound call: read to the end, checked in libp2p-ffi's order -- frame, delegation, operation,
 * limits -- and refused, because handing a call to the federation role is not built yet. Refusing
 * closes the stream at once, which is kinder to the caller's failover than letting it time out.
 */
async function answer(
  stream: Stream,
  connection: Connection,
  operations: readonly string[],
  limits: Limits,
  logger: Logger,
): Promise<void> {
  const frame = await readAll(stream)
  const request = frame === undefined ? undefined : parseRequest(frame)
  const delegation = request === undefined ? undefined : await verifyDelegation(request.delegation)
  const caller = connection.remotePeer.publicKey?.raw
  if (
    request === undefined ||
    delegation === undefined ||
    caller === undefined ||
    !sameBytes(delegation.node, caller) ||
    !operations.includes(request.protocol)
  ) {
    stream.abort(new Error('refused'))
    return
  }
  const address = connection.remoteAddr
  const refusal = limits.admit({
    group: delegation.group,
    peer: connection.remotePeer.toString(),
    ip: isRelayed(address) ? undefined : ipOf(address),
  })
  if (refusal !== undefined) {
    logger.debug(
      { cat: 'dht', event: 'dht.call.denied', data: { reason: refusal } },
      `call denied: ${refusal}`,
    )
    stream.abort(new Error('limited'))
    return
  }
  logger.warn(
    { cat: 'dht', event: 'dht.call.refused', data: { protocol: request.protocol } },
    `call on ${request.protocol} refused: no federation role to hand it to yet`,
  )
  stream.abort(new Error('rejected'))
}

/** The whole request, or undefined when it is larger than a request may be. */
async function readAll(stream: Stream): Promise<Uint8Array | undefined> {
  const parts: Uint8Array[] = []
  let size = 0
  for await (const chunk of stream) {
    const bytes = chunk instanceof Uint8Array ? chunk : chunk.subarray()
    size += bytes.length
    if (size > MAX_REQUEST_BYTES) {
      return undefined
    }
    parts.push(bytes)
  }
  return new Uint8Array(Buffer.concat(parts))
}

function isRelayed(address: Multiaddr): boolean {
  return address.toString().includes('/p2p-circuit')
}

function ipOf(address: Multiaddr): string | undefined {
  const [, family, host] = address.toString().split('/')
  return family === 'ip4' || family === 'ip6' ? host : undefined
}

function sameBytes(a: Uint8Array, b: Uint8Array): boolean {
  return Buffer.from(a).equals(Buffer.from(b))
}
