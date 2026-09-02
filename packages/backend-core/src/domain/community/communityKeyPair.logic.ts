import { generateKeyPairSync } from 'node:crypto'

/**
 * A fresh ed25519 key pair for a community, in the two shapes the columns hold.
 *
 * `contracts/db/communities.json` says `public_key bytes(32)` and `private_key bytes(64)`,
 * which is libsodium's layout and what legacy's dht-node writes: the secret key is the
 * 32-byte seed followed by the 32-byte public key. Storing it that way is what lets a
 * signature made here be verified by anything that reads the row, including the C path.
 *
 * **`node:crypto`, not a package.** `AGENTS.md`, section 13, is explicit that the strongest
 * measure is not adding a dependency, and this is the case it names: key generation on the
 * path that decides a community's identity. The JWK export is the only way node hands over
 * the raw scalars — `der` would wrap them in ASN.1 that we would then have to unwrap.
 *
 * This is *not* the SLIP-10 tree in `@gradido/shared`'s `CommunityKeyPair`. That one derives
 * member and account keys from a community root seed and belongs to the blockchain; this is
 * the community's own signing identity, which is what `communities.public_key` holds and
 * what federation identifies a community by. Two different keys with similar names — do not
 * merge them.
 */
export interface CommunityKeys {
  /** 32 bytes. */
  readonly publicKey: Uint8Array
  /** 64 bytes: seed then public key. Secret. */
  readonly privateKey: Uint8Array
}

export function newCommunityKeys(): CommunityKeys {
  const { publicKey, privateKey } = generateKeyPairSync('ed25519')
  const publicJwk = publicKey.export({ format: 'jwk' })
  const privateJwk = privateKey.export({ format: 'jwk' })

  if (publicJwk.x === undefined || privateJwk.d === undefined) {
    throw new Error('ed25519 key export did not yield raw scalars')
  }

  const publicBytes = Buffer.from(publicJwk.x, 'base64url')
  const seed = Buffer.from(privateJwk.d, 'base64url')
  if (publicBytes.length !== 32 || seed.length !== 32) {
    throw new Error(
      `ed25519 key has unexpected size: public ${publicBytes.length}, seed ${seed.length}`,
    )
  }

  return {
    publicKey: new Uint8Array(publicBytes),
    privateKey: new Uint8Array(Buffer.concat([seed, publicBytes])),
  }
}
