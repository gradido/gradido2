import type { Logger } from './logging/logger'

/**
 * What every Gradido service has, whatever else it has.
 *
 * The shutdown handler needs a logger and a way to let go of what the process holds, and
 * it must not need to know which service it is stopping -- the backend has a database
 * connection and the dht-node deliberately has none. Each service's own AppContext
 * satisfies this structurally; nothing has to inherit from it.
 */
export interface ServiceContext {
  readonly logger: Logger
  /** Releases what the process holds. Called on shutdown, not per request. */
  close(): Promise<void>
}
