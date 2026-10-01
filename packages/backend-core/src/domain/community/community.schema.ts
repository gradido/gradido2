import * as v from 'valibot'
import {
  atMostOneRowSchema,
  buffer32Schema,
  buffer64Schema,
  dateSchema,
  rowIdSchema,
  uint8Array32Schema,
  uint8Array64Schema,
  uuidv4Schema,
} from '../../database'

/**
 * This instance's own community, as the rest of the process refers to it -- and the shape the
 * row is read into, whichever database it came from.
 *
 * **The private key is deliberately not in it.** This value is held for the lifetime of the
 * process and read by anything that has a context — which is the last place a secret should
 * be. Whatever comes to need it (signing a federation handshake, signing a transaction)
 * loads it at that moment, through a repository method written for that purpose, and lets go
 * of it again. `contracts/logging.json` forbids logging it, and the cheapest way to keep
 * that promise is for the object everyone carries not to contain it.
 *
 * The uuid *is* here: it is the community's public identity and the thing federation names
 * it by. That it lives on this object and in `communities.community_uuid` — and in no other
 * table — is the whole point of `users.community_id` being a row id.
 */
export const homeCommunitySchema = v.pipe(
  v.object({
    /** `communities.id`. What every other table references. */
    id: rowIdSchema,
    communityUuid: uuidv4Schema,
    url: v.string(),
    /* Nullable in the contract, for communities known only by reference. */
    name: v.nullable(v.string(), ''),
    description: v.nullable(v.string()),
    /** ed25519 public key, 32 bytes. Public by name and by nature. */
    publicKey: uint8Array32Schema,
  }),
  v.readonly(),
)

export type HomeCommunityInput = v.InferInput<typeof homeCommunitySchema>
export type HomeCommunity = v.InferOutput<typeof homeCommunitySchema>

const SEVERAL_HOME_COMMUNITIES =
  'more than one home community: communities.remote = false on several rows'

/**
 * What the query for the rows with `remote = false` returned: the home community, or undefined
 * on a database that has never been set up. Two such rows are a broken database.
 */
export const homeCommunitySelectSchema = atMostOneRowSchema(
  homeCommunitySchema,
  SEVERAL_HOME_COMMUNITIES,
)

export type HomeCommunitySelectInput = v.InferInput<typeof homeCommunitySelectSchema>
export type HomeCommunitySelect = v.InferOutput<typeof homeCommunitySelectSchema>

/**
 * The same query for `private_key` alone: the ed25519 secret key, 64 bytes, or undefined on a
 * database that has never been set up.
 */
export const homeCommunitySigningKeySelectSchema = v.pipe(
  atMostOneRowSchema(
    v.object({
      /* Nullable in the table, for communities known only by reference. Never for this one. */
      privateKey: uint8Array64Schema,
    }),
    SEVERAL_HOME_COMMUNITIES,
  ),
  v.transform((row) => row?.privateKey),
)

export type HomeCommunitySigningKeySelectInput = v.InferInput<
  typeof homeCommunitySigningKeySelectSchema
>
export type HomeCommunitySigningKeySelect = v.InferOutput<
  typeof homeCommunitySigningKeySelectSchema
>

/**
 * The home community as it is written at first start, and the row that makes of it.
 *
 * `url`, `name` and `description` arrive as a parsed `HomeCommunitySetup`, which holds the
 * bounds of their columns. `remote` is not here: it is false in the statement itself, which is
 * what makes the row the home community.
 */
export const homeCommunityInsertSchema = v.pipe(
  v.object({
    url: v.string(),
    name: v.string(),
    description: v.nullable(v.string()),
    communityUuid: uuidv4Schema,
    publicKey: buffer32Schema,
    /** ed25519 secret key. Written once, never read back into a context. */
    privateKey: buffer64Schema,
    createdAt: dateSchema,
  }),
  v.transform((community) => ({
    ...community,
    /* When the community was founded, as far as this instance knows: now. Distinct from
       created_at, which is when this row was written — the two coincide only here. */
    creationDate: community.createdAt,
  })),
)

export type HomeCommunityInsertInput = v.InferInput<typeof homeCommunityInsertSchema>
export type HomeCommunityInsert = v.InferOutput<typeof homeCommunityInsertSchema>
