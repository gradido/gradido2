import type { Logger } from '@gradido/service-core'
import { deriveDhtNodeKeyPair, signDhtDelegation } from '@gradido/shared/crypto'
import { generateKeyPairFromSeed } from '@libp2p/crypto/keys'

/** What the node tests share: communities as `setup` leaves them, and a logger that records. */

export interface Logged {
  readonly event: string
  readonly message: string
}

export function recordingLogger(into: Logged[]): Logger {
  const record = (fields: { event: string }, message: string) => {
    into.push({ event: fields.event, message })
  }
  return {
    info: record,
    warn: record,
    debug: record,
    error: record,
    fatal: record,
    flush: () => undefined,
  } as unknown as Logger
}

/** A community and one instance of it, as `setup` would leave them: a master seed and its delegation. */
export async function community(communityByte: number, seedByte: number) {
  const communitySeed = new Uint8Array(32).fill(communityByte)
  const communityKey = (await generateKeyPairFromSeed('Ed25519', communitySeed)).publicKey.raw
  const masterSeed = new Uint8Array(32).fill(seedByte)
  const node = deriveDhtNodeKeyPair(masterSeed)
  const delegation = signDhtDelegation(
    new Uint8Array([...communitySeed, ...communityKey]),
    node.publicKey,
  )
  return {
    config: {
      MASTER_SEED: Buffer.from(masterSeed).toString('hex'),
      DHT_DELEGATION: Buffer.from(delegation).toString('hex'),
    },
    delegation,
  }
}

export async function eventually<T>(
  what: string,
  attempt: () => Promise<T | undefined>,
  ms = 15_000,
): Promise<T> {
  const deadline = Date.now() + ms
  for (;;) {
    const result = await attempt()
    if (result !== undefined) {
      return result
    }
    if (Date.now() > deadline) {
      throw new Error(`${what} did not happen within ${ms} ms`)
    }
    await Bun.sleep(200)
  }
}
