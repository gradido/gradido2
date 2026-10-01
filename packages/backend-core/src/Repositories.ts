import type { DatabaseConnection } from './database'
import {
  type CommunityRepository,
  CommunityRepositoryPostgresql,
  CommunityRepositorySqlite,
} from './domain'

/**
 * Every repository, constructed once for the database this process runs on.
 *
 * A repository holds its prepared statements and nothing else, so one of each serves every
 * request. An Interaction names the abstract class and never learns the dialect.
 */
export interface Repositories {
  readonly communities: CommunityRepository
}

/** The one place the dialect is chosen. */
export function createRepositories(db: DatabaseConnection): Repositories {
  if (db.kind === 'sqlite') {
    return { communities: new CommunityRepositorySqlite(db.drizzle) }
  }
  return { communities: new CommunityRepositoryPostgresql(db.drizzle) }
}
