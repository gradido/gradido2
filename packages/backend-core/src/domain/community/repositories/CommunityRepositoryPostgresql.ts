import { eq, sql } from 'drizzle-orm'
import type { BunSQLDatabase } from 'drizzle-orm/bun-sql/postgres'
import {
  columnsFor,
  communitiesPg,
  type DatabaseTransaction,
  PostgresqlStatements,
} from '../../../database'
import { type HomeCommunityInsert, homeCommunitySchema } from '../community.schema'
import { CommunityRepository } from './CommunityRepository'

/** The community statements in PostgreSQL, the reference dialect. */
export class CommunityRepositoryPostgresql extends CommunityRepository {
  private readonly statements: PostgresqlStatements

  public constructor(db: BunSQLDatabase) {
    super()
    this.statements = new PostgresqlStatements(db)
  }

  protected selectHomeCommunity(tx?: DatabaseTransaction) {
    return this.statements
      .prepared('community_home', tx, (executor) =>
        executor
          .select(columnsFor(communitiesPg, homeCommunitySchema))
          .from(communitiesPg)
          .where(eq(communitiesPg.remote, false))
          .limit(2),
      )
      .execute()
  }

  protected selectHomeCommunitySigningKey(tx?: DatabaseTransaction) {
    return this.statements
      .prepared('community_home_signing_key', tx, (executor) =>
        executor
          .select({ privateKey: communitiesPg.privateKey })
          .from(communitiesPg)
          .where(eq(communitiesPg.remote, false))
          .limit(2),
      )
      .execute()
  }

  protected async insertHomeCommunity(row: HomeCommunityInsert, tx?: DatabaseTransaction) {
    const [created] = await this.statements
      .prepared('community_home_insert', tx, (executor) =>
        executor
          .insert(communitiesPg)
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
          .returning({ id: communitiesPg.id }),
      )
      .execute(row)
    return created.id
  }
}
