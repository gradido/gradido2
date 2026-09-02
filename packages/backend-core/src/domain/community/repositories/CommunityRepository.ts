import { eq } from 'drizzle-orm'
import { communitiesPg, communitiesSqlite, type DatabaseConnection } from '../../../database'
import type { HomeCommunity, NewHomeCommunity } from '../community.data'

/**
 * How the community rows are loaded and persisted.
 *
 * Only the home community so far: every other row arrives through federation, which does not
 * exist yet. Both methods are startup-only, which is why neither is on a hot path and why
 * neither caches anything — the caller holds the result for the life of the process.
 *
 * Note what is *not* selected: `private_key`. It is a secret with exactly one future reader
 * (whatever signs a federation handshake), and until that exists nothing loads it. See
 * `community.data.ts`.
 */
export class CommunityRepository {
  public constructor(private readonly db: DatabaseConnection) {}

  /**
   * This instance's own community, or nothing on a database that has never been set up.
   *
   * `remote = false` on exactly one row, by contract. If there were ever two this would
   * quietly pick one, so it does not: a second home community is a broken database, not a
   * situation to cope with, and it is reported rather than tolerated.
   */
  public async findHomeCommunity(): Promise<HomeCommunity | undefined> {
    /* Two branches rather than one shared column list: a select() built from a union of
       PostgreSQL and SQLite columns is not a select() either driver will take. The same
       reason the rest of this package does it, one dialect at a time. */
    const rows =
      this.db.kind === 'sqlite'
        ? this.db.drizzle
            .select({
              id: communitiesSqlite.id,
              communityUuid: communitiesSqlite.communityUuid,
              url: communitiesSqlite.url,
              name: communitiesSqlite.name,
              description: communitiesSqlite.description,
              publicKey: communitiesSqlite.publicKey,
            })
            .from(communitiesSqlite)
            .where(eq(communitiesSqlite.remote, false))
            .limit(2)
            .all()
        : await this.db.drizzle
            .select({
              id: communitiesPg.id,
              communityUuid: communitiesPg.communityUuid,
              url: communitiesPg.url,
              name: communitiesPg.name,
              description: communitiesPg.description,
              publicKey: communitiesPg.publicKey,
            })
            .from(communitiesPg)
            .where(eq(communitiesPg.remote, false))
            .limit(2)

    const row = rows[0]
    if (row === undefined) {
      return undefined
    }
    if (rows.length > 1) {
      throw new Error('more than one home community: communities.remote = false on several rows')
    }
    return {
      id: BigInt(row.id),
      communityUuid: row.communityUuid,
      url: row.url,
      name: row.name ?? '',
      description: row.description,
      publicKey: new Uint8Array(row.publicKey),
    }
  }

  /** Writes the home community. Called once, at first start, and never again. */
  public async createHomeCommunity(community: NewHomeCommunity): Promise<bigint> {
    const row = {
      remote: false,
      url: community.url,
      publicKey: Buffer.from(community.publicKey),
      privateKey: Buffer.from(community.privateKey),
      communityUuid: community.communityUuid,
      name: community.name,
      description: community.description,
      /* When the community was founded, as far as this instance knows: now. Distinct from
         created_at, which is when this row was written — the two coincide only here. */
      creationDate: community.createdAt,
      createdAt: community.createdAt,
    }

    if (this.db.kind === 'sqlite') {
      const created = this.db.drizzle
        .insert(communitiesSqlite)
        .values(row)
        .returning({ id: communitiesSqlite.id })
        .get()
      return BigInt(created.id)
    }

    const [created] = await this.db.drizzle
      .insert(communitiesPg)
      .values(row)
      .returning({ id: communitiesPg.id })
    return created.id
  }
}
