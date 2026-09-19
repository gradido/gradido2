import { createPrivateKey, sign } from 'node:crypto'

/**
 * "This node key belongs to this community until then", signed by the community key: the
 * delegation every frame of the peer network carries. The format is libp2p-ffi's, byte for byte
 * -- its src/delegation.rs, and `contracts/test-vectors/master-seed.json` holds a delegation the
 * module signed itself.
 *
 * ```text
 * node key (32) | community key (32) | expires_ms, big endian (8, 0 = never) | signature (64)
 * signature: ed25519 by the community key over
 *            "libp2p-ffi delegation v1" || node key || community key || expires_ms
 * ```
 */
export const DHT_DELEGATION_BYTES = 136

const CONTEXT = Buffer.from('libp2p-ffi delegation v1', 'utf8')

/**
 * @p communityPrivateKey is the column's 64 bytes: seed, then public key.
 */
export function signDhtDelegation(
  communityPrivateKey: Uint8Array,
  nodeKey: Uint8Array,
  expiresMs = 0n,
): Uint8Array {
  if (communityPrivateKey.length !== 64 || nodeKey.length !== 32) {
    throw new Error('a delegation needs a 64-byte community private key and a 32-byte node key')
  }
  const seed = Buffer.from(communityPrivateKey.subarray(0, 32))
  const communityKey = Buffer.from(communityPrivateKey.subarray(32, 64))
  const expires = Buffer.alloc(8)
  expires.writeBigUInt64BE(expiresMs)

  const key = createPrivateKey({
    key: {
      kty: 'OKP',
      crv: 'Ed25519',
      d: seed.toString('base64url'),
      x: communityKey.toString('base64url'),
    },
    format: 'jwk',
  })
  const signature = sign(null, Buffer.concat([CONTEXT, nodeKey, communityKey, expires]), key)
  return new Uint8Array(Buffer.concat([nodeKey, communityKey, expires, signature]))
}
