import { publicKeyFromRaw } from '@libp2p/crypto/keys'
import { CID } from 'multiformats/cid'
import { sha256 } from 'multiformats/hashes/sha2'

/**
 * What travels between nodes, byte for byte what libp2p-ffi does -- its src/wire.rs and
 * src/delegation.rs. `libp2p-ffi/interop/js/lp2p.ts` is where this was first written and tested
 * against the module. DHT_RPC_PROTOCOL in contracts/const.json.
 *
 * ```text
 * request    1 | delegation (136) | u8 name length | protocol name | payload
 * response   1 | delegation (136) | payload          -- and an announcement
 * delegation node key (32) | group key (32) | expires_ms, big endian (8, 0 = never)
 *            | ed25519 signature by the group key over
 *              "libp2p-ffi delegation v1" || node key || group key || expires_ms
 * ```
 */
export const RPC_PROTOCOL = '/lp2p/rpc/1'
export const DELEGATION_BYTES = 136

const VERSION = 1
const DELEGATION_CONTEXT = new TextEncoder().encode('libp2p-ffi delegation v1')

export interface Delegation {
  readonly node: Uint8Array
  readonly group: Uint8Array
}

/** The delegation's keys if its signature holds and it has not expired, otherwise undefined. */
export async function verifyDelegation(
  bytes: Uint8Array,
  nowMs = Date.now(),
): Promise<Delegation | undefined> {
  if (bytes.length !== DELEGATION_BYTES) {
    return undefined
  }
  const node = bytes.slice(0, 32)
  const group = bytes.slice(32, 64)
  const expiresMs = new DataView(bytes.buffer, bytes.byteOffset + 64, 8).getBigUint64(0, false)
  if (expiresMs !== 0n && expiresMs <= BigInt(nowMs)) {
    return undefined
  }
  const signed = new Uint8Array(DELEGATION_CONTEXT.length + 72)
  signed.set(DELEGATION_CONTEXT)
  signed.set(bytes.subarray(0, 72), DELEGATION_CONTEXT.length)
  try {
    const valid = await publicKeyFromRaw(group).verify(signed, bytes.subarray(72))
    return valid ? { node, group } : undefined
  } catch {
    return undefined
  }
}

export interface Request {
  readonly delegation: Uint8Array
  readonly protocol: string
  readonly payload: Uint8Array
}

export function parseRequest(frame: Uint8Array): Request | undefined {
  const start = 2 + DELEGATION_BYTES
  if (frame.length < start || frame[0] !== VERSION) {
    return undefined
  }
  const nameLength = frame[1 + DELEGATION_BYTES] as number
  if (nameLength === 0 || frame.length < start + nameLength) {
    return undefined
  }
  return {
    delegation: frame.subarray(1, 1 + DELEGATION_BYTES),
    protocol: new TextDecoder().decode(frame.subarray(start, start + nameLength)),
    payload: frame.subarray(start + nameLength),
  }
}

/** A response, or an announcement: version | delegation | payload. */
export function parseResponse(
  frame: Uint8Array,
): { delegation: Uint8Array; payload: Uint8Array } | undefined {
  if (frame.length < 1 + DELEGATION_BYTES || frame[0] !== VERSION) {
    return undefined
  }
  return {
    delegation: frame.subarray(1, 1 + DELEGATION_BYTES),
    payload: frame.subarray(1 + DELEGATION_BYTES),
  }
}

/** The Kademlia key a group's nodes provide under: CIDv1(raw, sha2-256(group key)). */
export async function providerKey(group: Uint8Array): Promise<CID> {
  return CID.createV1(0x55, await sha256.digest(group))
}
