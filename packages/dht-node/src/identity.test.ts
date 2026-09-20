import { describe, expect, test } from 'bun:test'
import { IdentityError, loadIdentity } from './identity'

const seed = '07'.repeat(32)
// master-seed.delegate.never-expires in contracts/test-vectors/master-seed.json: signed by
// libp2p-ffi for the node that seed derives.
const delegation =
  '6d238a55b43ae445b24c6fb157fb2202ee9355eb26fb45ad57768ee902eeb3c3b533d8ad9fcfbdde0b481c1b334ddc3c53412fd614564e7e5afd020368d382c30000000000000000c2ad260d5a267e793d5989f7ff90785f8efc91b583e073fb3b3c8a0174601b047cc84b00eab6277e6c151f6ed03bb793a8993f294f823507cae2631cdfd2860a'

async function failure(config: { MASTER_SEED: string; DHT_DELEGATION: string }): Promise<string> {
  try {
    await loadIdentity(config)
    return 'none'
  } catch (error) {
    return error instanceof IdentityError ? error.reason : String(error)
  }
}

describe('loadIdentity', () => {
  test('derives the node the delegation names, and the community behind it', async () => {
    const identity = await loadIdentity({ MASTER_SEED: seed, DHT_DELEGATION: delegation })
    expect(Buffer.from(identity.nodeKey).toString('hex')).toBe(delegation.slice(0, 64))
    expect(Buffer.from(identity.group).toString('hex')).toBe(delegation.slice(64, 128))
    expect(Buffer.from(identity.privateKey.publicKey.raw).toString('hex')).toBe(
      delegation.slice(0, 64),
    )
  })

  test('names what is wrong with the reason the C path writes', async () => {
    expect(await failure({ MASTER_SEED: '', DHT_DELEGATION: delegation })).toBe(
      'master-seed-missing',
    )
    expect(await failure({ MASTER_SEED: 'xyz', DHT_DELEGATION: delegation })).toBe(
      'master-seed-invalid',
    )
    expect(await failure({ MASTER_SEED: seed, DHT_DELEGATION: '' })).toBe('delegation-missing')
    expect(await failure({ MASTER_SEED: seed, DHT_DELEGATION: 'abcd' })).toBe('delegation-invalid')
    expect(await failure({ MASTER_SEED: 'ff'.repeat(32), DHT_DELEGATION: delegation })).toBe(
      'delegation-foreign',
    )
    const forged = `${delegation.slice(0, 200)}${delegation[200] === '0' ? '1' : '0'}${delegation.slice(201)}`
    expect(await failure({ MASTER_SEED: seed, DHT_DELEGATION: forged })).toBe('delegation-invalid')
  })
})
