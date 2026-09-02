import type { Logger } from '@gradido/service-core'
import type { DatabaseConnection } from './database'
import type { HomeCommunity } from './domain'

/**
 * What exists before this instance knows which community it is.
 *
 * Startup happens in an order: the database is opened and migrated, and only then can the
 * home community be read — or, on an empty database, asked for and written. The code that
 * does that cannot ask for a context that already contains the answer it is producing, so
 * the context is split rather than made optional. A `homeCommunity?: HomeCommunity` would
 * push the question into every request handler that can never actually see it missing.
 */
export interface DatabaseContext {
  readonly logger: Logger
  readonly db: DatabaseConnection
}

/**
 * What an Interaction serving a request is allowed to reach.
 *
 * `@gradido/backend`'s AppContext satisfies it structurally — nothing inherits from it — so
 * the domain code says in its signature what it touches, and a test can hand it a database
 * and a logger without building an application. Deliberately *not* `ServiceContext`: an
 * Interaction has no business closing the process's connections.
 *
 * `homeCommunity` is here rather than looked up per request because it is the definition of
 * static data (`AGENTS.md`, section 9): one row, written once at setup, changed only by an
 * admin renaming the community. It also cannot be missing — the process refuses to start
 * without it — so nothing downstream has to handle its absence. It carries no private key;
 * see `domain/community/community.data.ts` for why.
 *
 * It grows with the application: the session map, the global caches and the clients for
 * external services described in `Architecture.md` belong here as they are written, and an
 * Interaction that needs one of them will say so by reading it from here.
 */
export interface BackendContext extends DatabaseContext {
  readonly homeCommunity: HomeCommunity
}
