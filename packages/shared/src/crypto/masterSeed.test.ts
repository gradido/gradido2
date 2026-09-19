import { describe, expect, it } from 'bun:test'
import {
  deriveDhtNodeKeyPair,
  MASTER_SEED_BYTES,
  newMasterSeed,
  parseMasterSeed,
} from './masterSeed'

// What the seed derives and which text is one: contracts/test-vectors/master-seed.json, run by
// packages/contract-tests. This is what the file cannot say -- a new seed is random.
describe('newMasterSeed', () => {
  it('is a seed the derivation takes', () => {
    const seed = newMasterSeed('')
    expect(seed.length).toBe(MASTER_SEED_BYTES)
    expect(() => deriveDhtNodeKeyPair(seed)).not.toThrow()
  })

  it('differs every time, whatever was typed', () => {
    expect(newMasterSeed('same')).not.toEqual(newMasterSeed('same'))
  })

  it('round-trips through the hex setup writes', () => {
    const seed = newMasterSeed('keys')
    expect(parseMasterSeed(Buffer.from(seed).toString('hex'))).toEqual(seed)
  })
})

describe('deriveDhtNodeKeyPair', () => {
  it('refuses a seed of the wrong length rather than deriving from it', () => {
    expect(() => deriveDhtNodeKeyPair(new Uint8Array(16))).toThrow('32 bytes')
  })
})
