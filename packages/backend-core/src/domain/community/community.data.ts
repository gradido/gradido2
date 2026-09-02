/**
 * The domain state of a community. `contracts/db/communities.json` is the authority on what
 * a row holds; this is what the code that reads one passes around.
 */

/**
 * This instance's own community, as the rest of the process refers to it.
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
export interface HomeCommunity {
  /** `communities.id`. What every other table references. */
  readonly id: bigint
  readonly communityUuid: string
  readonly url: string
  readonly name: string
  readonly description: string | null
  /** ed25519 public key, 32 bytes. Public by name and by nature. */
  readonly publicKey: Uint8Array
}

/** Everything the home community is written from at first start. */
export interface NewHomeCommunity {
  readonly url: string
  readonly name: string
  readonly description: string | null
  readonly communityUuid: string
  readonly publicKey: Uint8Array
  /** ed25519 secret key, 64 bytes. Written once, never read back into a context. */
  readonly privateKey: Uint8Array
  readonly createdAt: Date
}
