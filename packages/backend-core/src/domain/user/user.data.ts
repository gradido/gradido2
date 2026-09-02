import type { Language } from '@gradido/shared/schemas'

/**
 * The domain state of a member account. `contracts/db/users.json` and
 * `contracts/db/user_contacts.json` are the authority on what a row holds; this is what the
 * code that creates one passes around.
 */

/** Everything an account is written from, decided before the first row is touched. */
export interface NewAccount {
  /** Trimmed and lowercased. The comparison in `user_contacts.email` is exact. */
  readonly email: string
  readonly firstName: string
  readonly lastName: string
  readonly language: Language
  /** `users.community_id` — a row id, never the community's uuid. */
  readonly communityId: bigint
  /** `users.gradido_id`. Made by the Interaction, not by the database, and free in that
      community: `users_uuid_key` is `(gradido_id, community_id)`. */
  readonly gradidoId: string
  /**
   * `user_contacts.email_verification_code`. A secret: never logged, and never in a response
   * — which `user.create` makes easy, since it has no response body at all.
   */
  readonly emailVerificationCode: bigint
  /** One instant for both rows, so the account has a single moment of creation. */
  readonly createdAt: Date
}

/**
 * Where the login address leads.
 *
 * Deliberately not the whole account: what asks for it is the check whether an address is
 * already in use, and that answer must not turn into a way to read a stranger's profile.
 * The name and language are here because the mail that goes out in that case is addressed to
 * the member who *owns* the address, in their language — never in the new registrant's.
 */
export interface AddressOwner {
  readonly id: bigint
  readonly firstName: string
  readonly lastName: string
  readonly language: string
}

/** The one normalization every lookup and every write agrees on. */
export function normalizeEmail(email: string): string {
  return email.trim().toLowerCase()
}
