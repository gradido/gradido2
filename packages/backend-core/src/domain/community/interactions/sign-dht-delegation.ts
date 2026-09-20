import { deriveDhtNodeKeyPair, signDhtDelegation } from '@gradido/shared/crypto'
import type { DatabaseContext } from '../../../BackendContext'
import { CommunityRepository } from '../repositories'

/**
 * The community vouches for this instance's dht node: its key, derived from @p masterSeed along
 * `dht`, signed by the community key.
 *
 * Signed by `setup`, which is the one place that has both -- the key is in the community row and
 * the dht-node role reads no database. What it returns goes into the configuration the role
 * starts from. Never expires: a delegation is replaced by running `setup` again, not by waiting.
 */
export async function signDhtDelegationFor(
  context: DatabaseContext,
  masterSeed: Uint8Array,
): Promise<Uint8Array> {
  const signingKey = await new CommunityRepository(context.db).findHomeCommunitySigningKey()
  if (signingKey === undefined) {
    throw new Error('there is no home community to sign a delegation with')
  }
  try {
    return signDhtDelegation(signingKey, deriveDhtNodeKeyPair(masterSeed).publicKey)
  } finally {
    signingKey.fill(0)
  }
}
