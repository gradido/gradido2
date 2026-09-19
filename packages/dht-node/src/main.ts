import { Logger, registerPeerNetwork, setupGracefulShutdown } from '@gradido/service-core'
import { CONFIG } from './config'
import { IdentityError, loadIdentity } from './identity'
import { startNode } from './node'

/**
 * The dht-node role, from the command line down. A function rather than a file that runs on
 * import, because the single binary starts it too -- see `packages/bundle`.
 *
 * `fast-servers/dht-node/src/dht_node_server.c` is the same role on the fast path: the same
 * variables, the same checks in the same order, the same events.
 */
export async function runDhtNode(_argv: readonly string[]): Promise<void> {
  const logger = Logger.create(CONFIG)

  if (CONFIG.DHT_TOPIC === '') {
    /* No topic, no network, as in legacy. Asking for the role without configuring it is a
       mistake worth a fatal rather than a process that sits there looking healthy. */
    logger.fatal(
      { cat: 'dht', event: 'dht.node.failed', data: { reason: 'topic-missing' } },
      'dht-node needs DHT_TOPIC, which is unset',
    )
    logger.flush()
    process.exit(1)
  }

  let identity: Awaited<ReturnType<typeof loadIdentity>>
  try {
    identity = await loadIdentity(CONFIG)
  } catch (error) {
    if (!(error instanceof IdentityError)) {
      throw error
    }
    logger.fatal(
      { cat: 'dht', event: 'dht.node.failed', data: { reason: error.reason } },
      error.message,
    )
    logger.flush()
    process.exit(1)
  }

  const reachability = CONFIG.DHT_REACHABILITY
  let node: Awaited<ReturnType<typeof startNode>>
  try {
    node = await startNode({
      topic: CONFIG.DHT_TOPIC,
      port: Number(CONFIG.DHT_PORT),
      public: reachability === 'public',
      identity,
      logger,
      bootstrapUrl: CONFIG.DHT_BOOTSTRAP_URL === '' ? undefined : CONFIG.DHT_BOOTSTRAP_URL,
    })
  } catch (error) {
    logger.fatal(
      { cat: 'dht', event: 'dht.node.failed', data: { reason: 'network' } },
      `the network node did not start: ${String(error)}`,
    )
    logger.flush()
    process.exit(1)
  }

  /* For the roles beside it in this process: the backend answers peer.bootstrap from it. */
  registerPeerNetwork(node)
  const nodeKey = Buffer.from(identity.nodeKey).toString('hex')
  logger.info(
    {
      cat: 'dht',
      event: 'dht.node.started',
      data: { nodeKey, port: Number(CONFIG.DHT_PORT), reachability },
    },
    `dht-node ${nodeKey} is up on '/gradido/${CONFIG.DHT_TOPIC}/kad/1', port ${CONFIG.DHT_PORT}, ${reachability}`,
  )

  setupGracefulShutdown(
    {
      logger,
      async close() {
        /* The node holds no database and nothing else to release. */
      },
    },
    async () => {
      /* js-libp2p does not finish stopping when the peer closes the same connection at the same
         moment; `complete` says whether the process exit closes the rest. */
      registerPeerNetwork(undefined)
      const complete = await node.stop()
      logger.info(
        { cat: 'dht', event: 'dht.node.stopped', data: { complete } },
        complete ? 'dht-node stopped' : 'dht-node left to the process exit',
      )
    },
  )
}
