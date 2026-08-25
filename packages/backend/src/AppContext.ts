import { connectDatabase, type DatabaseConnection } from '@gradido/backend-core'
import type { Logger, ServiceContext } from '@gradido/service-core'
import { CONFIG } from './config'

/**
 * What the whole process shares: everything that was a singleton in gradido legacy.
 *
 * It is passed down explicitly instead of being reachable from anywhere, so what a piece
 * of code touches is visible in its signature. It holds the database connection and the
 * logger today; the global caches, the session map and the clients for external services
 * described in Architecture.md, AppContext belong here as they are written.
 *
 * Everything in here must be safe to lose: the database is the truth, this is the working
 * view of it, and a restart must cost nothing but a cold cache.
 */
export class AppContext implements ServiceContext {
  public readonly logger: Logger
  public readonly db: DatabaseConnection

  /**
   * The logger is handed in rather than created here: opening the database can fail, and
   * whoever reports that failure needs a logger that exists before this constructor runs.
   */
  public constructor(logger: Logger) {
    this.logger = logger
    this.db = connectDatabase(CONFIG)
  }

  /** Releases what the process holds. Called by the shutdown handler, not per request. */
  public async close(): Promise<void> {
    await this.db.close()
    this.logger.flush()
  }
}
