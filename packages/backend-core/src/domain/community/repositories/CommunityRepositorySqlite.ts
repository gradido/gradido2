import { eq, sql } from 'drizzle-orm'
import type { SQLiteBunDatabase } from 'drizzle-orm/bun-sqlite'
import { columnsFor, communitiesSqlite, SqliteStatements } from '../../../database'
import { type HomeCommunityInsert, homeCommunitySchema } from '../community.schema'
import { CommunityRepository } from './CommunityRepository'

/**
 * The community statements in SQLite.
 *
 * No method takes the transaction of the signature it implements: SQLite has one connection,
 * and a statement prepared on it runs inside whichever transaction is open.
 */
export class CommunityRepositorySqlite extends CommunityRepository {
  private readonly statements: SqliteStatements

  public constructor(db: SQLiteBunDatabase) {
    super()
    this.statements = new SqliteStatements(db)
  }

  protected async selectHomeCommunity() {
    return this.statements
      .prepared('community_home', (db) =>
        db
          .select(columnsFor(communitiesSqlite, homeCommunitySchema))
          .from(communitiesSqlite)
          .where(eq(communitiesSqlite.remote, false))
          .limit(2),
      )
      .all()
  }

  protected async selectHomeCommunitySigningKey() {
    return this.statements
      .prepared('community_home_signing_key', (db) =>
        db
          .select({ privateKey: communitiesSqlite.privateKey })
          .from(communitiesSqlite)
          .where(eq(communitiesSqlite.remote, false))
          .limit(2),
      )
      .all()
  }

  protected async insertHomeCommunity(row: HomeCommunityInsert) {
    return this.statements
      .prepared('community_home_insert', (db) =>
        db
          .insert(communitiesSqlite)
          .values({
            remote: false,
            url: sql.placeholder('url'),
            publicKey: sql.placeholder('publicKey'),
            privateKey: sql.placeholder('privateKey'),
            communityUuid: sql.placeholder('communityUuid'),
            name: sql.placeholder('name'),
            description: sql.placeholder('description'),
            creationDate: sql.placeholder('creationDate'),
            createdAt: sql.placeholder('createdAt'),
          })
          .returning({ id: communitiesSqlite.id }),
      )
      .get(row).id
  }
}
