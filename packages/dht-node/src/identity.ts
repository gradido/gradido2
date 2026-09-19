import { deriveDhtNodeKeyPair, parseMasterSeed } from '@gradido/shared/crypto'
import { generateKeyPairFromSeed } from '@libp2p/crypto/keys'
import type { Ed25519PrivateKey } from '@libp2p/interface'
import { DELEGATION_BYTES, verifyDelegation } from './wire'

/**
 * The node's identity: its key, derived from MASTER_SEED along `dht`, and the delegation `setup`
 * had the community key sign for it -- Architecture.md, *Communities and instances*.
 * `fast-servers/dht-node/src/dht_node_server.c`, load_identity, is the other path, and a failure
 * here is reported as the same dht.node.failed reason it writes.
 */

/** The `reason` of dht.node.failed in contracts/logging.json that a failure here is reported as. */
export type IdentityFailure =
  | 'master-seed-missing'
  | 'master-seed-invalid'
  | 'delegation-missing'
  | 'delegation-invalid'
  | 'delegation-foreign'

export class IdentityError extends Error {
  public constructor(
    public readonly reason: IdentityFailure,
    message: string,
  ) {
    super(message)
  }
}

export interface Identity {
  readonly privateKey: Ed25519PrivateKey
  /** The node's ed25519 public key: what its peer id is made of and what the delegation names. */
  readonly nodeKey: Uint8Array
  /** The community key the delegation names. */
  readonly group: Uint8Array
  readonly delegation: Uint8Array
}

export async function loadIdentity(config: {
  readonly MASTER_SEED: string
  readonly DHT_DELEGATION: string
}): Promise<Identity> {
  if (config.MASTER_SEED === '') {
    throw new IdentityError(
      'master-seed-missing',
      'dht-node needs MASTER_SEED, which `setup` makes',
    )
  }
  const masterSeed = parseMasterSeed(config.MASTER_SEED)
  if (masterSeed === undefined) {
    throw new IdentityError('master-seed-invalid', 'dht-node needs MASTER_SEED as 64 hex digits')
  }
  const node = deriveDhtNodeKeyPair(masterSeed)
  masterSeed.fill(0)

  if (config.DHT_DELEGATION === '') {
    throw new IdentityError(
      'delegation-missing',
      "dht-node needs DHT_DELEGATION: run `setup`, which has the community key sign this instance's node key",
    )
  }
  if (!new RegExp(`^[0-9a-fA-F]{${2 * DELEGATION_BYTES}}$`, 'u').test(config.DHT_DELEGATION)) {
    throw new IdentityError(
      'delegation-invalid',
      `DHT_DELEGATION is not ${2 * DELEGATION_BYTES} hex digits`,
    )
  }
  const delegation = new Uint8Array(Buffer.from(config.DHT_DELEGATION, 'hex'))
  /* The one way the two drift apart in practice: a seed replaced after `setup` signed. */
  if (!Buffer.from(delegation.subarray(0, 32)).equals(Buffer.from(node.publicKey))) {
    throw new IdentityError(
      'delegation-foreign',
      'DHT_DELEGATION names another node than MASTER_SEED derives: run `setup` again to have this one signed',
    )
  }
  if ((await verifyDelegation(delegation)) === undefined) {
    throw new IdentityError(
      'delegation-invalid',
      'DHT_DELEGATION does not verify: its signature is not the community key’s, or it has expired',
    )
  }

  const seed = node.seed
  const privateKey = await generateKeyPairFromSeed('Ed25519', seed)
  seed.fill(0)
  return { privateKey, nodeKey: node.publicKey, group: delegation.slice(32, 64), delegation }
}
