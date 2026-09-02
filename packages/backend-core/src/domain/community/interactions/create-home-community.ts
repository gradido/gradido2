import type { HomeCommunitySetup } from '@gradido/shared/schemas'
import type { DatabaseContext } from '../../../BackendContext'
import type { HomeCommunity } from '../community.data'
import { newCommunityKeys } from '../communityKeyPair.logic'
import { CommunityRepository } from '../repositories'

/**
 * The instance becomes a community.
 *
 * This runs once, at the first start against an empty database, and everything that follows
 * depends on it: `users.community_id` is NOT NULL, so no member can exist before this row
 * does, and the backend refuses to serve without it.
 *
 * Legacy has the dht-node do this, out of `COMMUNITY_NAME` and `COMMUNITY_DESCRIPTION` in
 * the environment. gradido2 asks instead — see `@gradido/backend`'s `setup/` — and the
 * difference is not cosmetic: an environment variable is read at every start, so it is a
 * *setting*, and a community's name and key pair are not settings. They are written once and
 * then referred to. An env var that silently disagrees with the row it created is a class of
 * confusion that does not need to exist.
 *
 * The caller supplies only what a person knows. Everything else is made here:
 *
 * ```text
 * community_uuid   the community's public identity, and the only place a community uuid
 *                  lives — users reference the community by row id, see contracts/db/users
 * public_key       ed25519, what federation identifies this community by
 * private_key      the other half. Written and then left alone: it is not put into the
 *                  HomeCommunity this returns, because that value is held for the life of
 *                  the process and read by anything holding a context
 * creation_date    now, as far as this instance can know
 * ```
 */
export async function createHomeCommunity(
  context: DatabaseContext,
  setup: HomeCommunitySetup,
): Promise<HomeCommunity> {
  const communities = new CommunityRepository(context.db)

  /* Not a draw-and-check like users.gradido_id: communities_uuid_key is a plain unique index
     on one column, so the database is the check. Legacy loops here because its equivalent
     index is the same shape and it chose to look first anyway. */
  const communityUuid = crypto.randomUUID()
  const keys = newCommunityKeys()

  const id = await communities.createHomeCommunity({
    url: setup.url,
    name: setup.name,
    description: setup.description,
    communityUuid,
    publicKey: keys.publicKey,
    privateKey: keys.privateKey,
    createdAt: new Date(),
  })

  context.logger.info(
    {
      cat: 'community',
      event: 'community.home.created',
      data: { uuid: communityUuid, url: setup.url },
    },
    `home community "${setup.name}" created`,
  )

  return {
    id,
    communityUuid,
    url: setup.url,
    name: setup.name,
    description: setup.description,
    publicKey: keys.publicKey,
  }
}
