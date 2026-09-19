import { describe, expect, test } from 'bun:test'
import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { parseRequest, providerKey, verifyDelegation } from './wire'

// Delegations libp2p-ffi signed itself, from the contract vectors.
const vectors = JSON.parse(
  readFileSync(
    join(import.meta.dirname, '../../../contracts/test-vectors/master-seed.json'),
    'utf8',
  ),
).vectors as {
  id: string
  expect: { delegation?: string; communityKey?: string; dhtNodeKey?: string }
}[]
const vector = (id: string) => vectors.find((v) => v.id === id)?.expect ?? {}
const bytes = (hex: string) => new Uint8Array(Buffer.from(hex, 'hex'))

describe('verifyDelegation', () => {
  test('accepts what libp2p-ffi signed, and names its keys', async () => {
    const expected = vector('master-seed.delegate.never-expires')
    const delegation = await verifyDelegation(bytes(expected.delegation as string))
    expect(Buffer.from(delegation?.node ?? []).toString('hex')).toBe(expected.dhtNodeKey as string)
    expect(Buffer.from(delegation?.group ?? []).toString('hex')).toBe(
      expected.communityKey as string,
    )
  })

  test('refuses one changed byte', async () => {
    const delegation = bytes(vector('master-seed.delegate.never-expires').delegation as string)
    delegation[3] = (delegation[3] as number) ^ 1
    expect(await verifyDelegation(delegation)).toBeUndefined()
  })

  test('honours the expiry, and only the expiry', async () => {
    const delegation = bytes(vector('master-seed.delegate.expires').delegation as string)
    const expires = Number(new DataView(delegation.buffer).getBigUint64(64, false))
    expect(await verifyDelegation(delegation, expires - 1)).toBeDefined()
    expect(await verifyDelegation(delegation, expires)).toBeUndefined()
  })

  test('refuses the wrong length', async () => {
    expect(await verifyDelegation(new Uint8Array(135))).toBeUndefined()
  })
})

describe('parseRequest', () => {
  const delegation = new Uint8Array(136).fill(9)
  const name = new TextEncoder().encode('/gradido/test/1')

  test('takes the frame apart', () => {
    const request = parseRequest(new Uint8Array([1, ...delegation, name.length, ...name, 7, 8]))
    expect(request?.protocol).toBe('/gradido/test/1')
    expect([...(request?.payload ?? [])]).toEqual([7, 8])
  })

  test('refuses another version, an empty name and a truncated one', () => {
    expect(parseRequest(new Uint8Array([2, ...delegation, name.length, ...name]))).toBeUndefined()
    expect(parseRequest(new Uint8Array([1, ...delegation, 0]))).toBeUndefined()
    expect(parseRequest(new Uint8Array([1, ...delegation, name.length, 1]))).toBeUndefined()
  })
})

describe('providerKey', () => {
  test('is the multihash libp2p-ffi provides under', async () => {
    // provider_key(&[1; 32]) in libp2p-ffi's src/wire.rs test.
    const key = await providerKey(new Uint8Array(32).fill(1))
    expect(Buffer.from(key.multihash.bytes).toString('hex')).toBe(
      '122072cd6e8422c407fb6d098690f1130b7ded7ec2f7f5e1d30bd9d521f015363793',
    )
  })
})
