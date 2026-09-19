import { randomBytes } from 'node:crypto'
import { signKeyPairDerive, signKeyPairGenerateFromSeed } from '@gradido/shared-native'
import { hashGeneric } from './hash'
import { SignKeyPair } from './Sign'

/**
 * The root of every key this instance derives rather than stores -- `MASTER_SEED` in
 * `contracts/secrets.json`.
 *
 * A SLIP-10 tree through gradido-blockchain-core, the same C function the fast path calls, so the
 * two implementations cannot derive different identities from one seed. The contract vectors in
 * `contracts/test-vectors/master-seed.json` hold both to that.
 */

/** Length of a master seed -- MASTER_SEED_BYTES in contracts/const.json. */
export const MASTER_SEED_BYTES = 32

/**
 * The SLIP-10 index of the path segment `dht`: the word's ASCII bytes read as one big-endian
 * number -- MASTER_SEED_PATH_DHT in contracts/const.json.
 */
export const MASTER_SEED_PATH_DHT = 0x00646874

/**
 * A new master seed: BLAKE2b-256 over 32 bytes of the operating system's randomness, the wall
 * clock, the monotonic clock and whatever the operator typed.
 *
 * The operating system's randomness is what makes it secret. The clocks and the typing only stand
 * in for it should that source ever be broken, which is why none of them may be the whole of it.
 */
export function newMasterSeed(typed: string): Uint8Array {
  const clocks = Buffer.alloc(16)
  clocks.writeBigUInt64BE(BigInt(Date.now()), 0)
  clocks.writeBigUInt64BE(process.hrtime.bigint(), 8)
  const input = Buffer.concat([randomBytes(32), clocks, Buffer.from(typed, 'utf8')])
  return new Uint8Array(hashGeneric(input))
}

/**
 * @p text as a master seed, or undefined when it is not 64 hex digits.
 *
 * Only the length and the digits are checked: every 32 bytes are a valid seed.
 */
export function parseMasterSeed(text: string): Uint8Array | undefined {
  if (!/^[0-9a-fA-F]{64}$/u.test(text)) {
    return undefined
  }
  return new Uint8Array(Buffer.from(text, 'hex'))
}

/**
 * This instance's dht-node identity: one hardened step along `dht` from the master seed's root.
 * Its `seed` is what libp2p-ffi takes as the node seed; its public key is the node key a
 * delegation names.
 */
export function deriveDhtNodeKeyPair(masterSeed: Uint8Array): SignKeyPair {
  if (masterSeed.length !== MASTER_SEED_BYTES) {
    throw new Error(`a master seed is ${MASTER_SEED_BYTES} bytes, got ${masterSeed.length}`)
  }
  const root = signKeyPairGenerateFromSeed(masterSeed)
  return new SignKeyPair(signKeyPairDerive(root, MASTER_SEED_PATH_DHT))
}
