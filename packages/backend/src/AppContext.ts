import type { BackendContext, DatabaseConnection, HomeCommunity } from '@gradido/backend-core'
import type { Logger, ServiceContext } from '@gradido/service-core'

/**
 * What the whole process shares: everything that was a singleton in gradido legacy.
 *
 * It is passed down explicitly instead of being reachable from anywhere, so what a piece
 * of code touches is visible in its signature. It holds the database connection, the logger
 * and the community this instance is; the global caches, the session map and the clients for
 * external services described in Architecture.md, AppContext belong here as they are written.
 *
 * Everything in here must be safe to lose: the database is the truth, this is the working
 * view of it, and a restart must cost nothing but a cold cache — including `homeCommunity`,
 * which is read back off the one row that holds it.
 *
 * It satisfies `BackendContext` structurally, which is what an Interaction asks for, and
 * `ServiceContext`, which is what the shutdown handler asks for. It inherits from neither.
 */
export class AppContext implements ServiceContext, BackendContext {
  /**
   * Everything is handed in rather than built here, because startup has an order: the
   * database has to answer and be migrated before the community can be read off it, and
   * whoever reports a failure on the way needs a logger that already exists. A constructor
   * that did all of that could not be one.
   */
  public constructor(
    public readonly logger: Logger,
    public readonly db: DatabaseConnection,
    public readonly homeCommunity: HomeCommunity,
  ) {}

  /** Releases what the process holds. Called by the shutdown handler, not per request. */
  public async close(): Promise<void> {
    await this.db.close()
    this.logger.flush()
  }
}
