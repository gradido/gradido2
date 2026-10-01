import * as v from 'valibot'
import {
  type DatabaseTransaction,
  parseSecret,
  type RowIdInput,
  rowIdSchema,
} from '../../../database'
import {
  type HomeCommunityInsert,
  type HomeCommunityInsertInput,
  type HomeCommunitySelect,
  type HomeCommunitySelectInput,
  type HomeCommunitySigningKeySelect,
  homeCommunityInsertSchema,
  homeCommunitySelectSchema,
  homeCommunitySigningKeySelectSchema,
} from '../community.schema'

/**
 * How the community rows are loaded and persisted.
 *
 * Abstract, with one subclass per dialect. The subclass holds the statements, each prepared
 * the first time it runs; this class holds what an answer means -- how many rows are
 * acceptable, and the valibot schema that turns a row of either database into the one value
 * the domain works with. A subclass therefore returns its rows as the driver gave them.
 *
 * Every method takes an optional transaction. Without one the statement runs on its own; with
 * one it is part of what `DatabaseConnection.transaction` commits or rolls back.
 *
 * Only the home community so far: every other row arrives through federation, which does not
 * exist yet.
 */
export abstract class CommunityRepository {
  /**
   * This instance's own community, or nothing on a database that has never been set up.
   * Without `private_key` -- see `community.schema.ts`.
   */
  public async findHomeCommunity(tx?: DatabaseTransaction): Promise<HomeCommunitySelect> {
    return v.parse(homeCommunitySelectSchema, await this.selectHomeCommunity(tx))
  }

  /**
   * The home community's private key, 64 bytes, or nothing on a database that has never been set
   * up. For signing, by a caller that lets go of it afterwards.
   */
  public async findHomeCommunitySigningKey(
    tx?: DatabaseTransaction,
  ): Promise<HomeCommunitySigningKeySelect> {
    return parseSecret(
      homeCommunitySigningKeySelectSchema,
      await this.selectHomeCommunitySigningKey(tx),
    )
  }

  /** Writes the home community and answers its row id. Called once, at first start. */
  public async createHomeCommunity(
    community: HomeCommunityInsertInput,
    tx?: DatabaseTransaction,
  ): Promise<bigint> {
    const row = parseSecret(homeCommunityInsertSchema, community)
    return v.parse(rowIdSchema, await this.insertHomeCommunity(row, tx))
  }

  /** The rows with `remote = false`, at most two: the second is only there to be noticed. */
  protected abstract selectHomeCommunity(
    tx?: DatabaseTransaction,
  ): Promise<HomeCommunitySelectInput>

  /** `private_key` of the rows with `remote = false`, at most two. Null as the column allows. */
  protected abstract selectHomeCommunitySigningKey(
    tx?: DatabaseTransaction,
  ): Promise<{ privateKey: Uint8Array | null }[]>

  /** Inserts @p row with `remote = false` and answers the id it was given. */
  protected abstract insertHomeCommunity(
    row: HomeCommunityInsert,
    tx?: DatabaseTransaction,
  ): Promise<RowIdInput>
}
