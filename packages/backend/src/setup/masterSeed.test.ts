import { afterEach, describe, expect, test } from 'bun:test'
import { askForMasterSeed } from './masterSeed'
import { SetupError } from './requireHomeCommunity'

// The branch that makes a new seed asks on a terminal and is not reached here: a seed is made
// only when none is configured, and these cases all configure one.
describe('askForMasterSeed', () => {
  const saved = process.env.MASTER_SEED

  afterEach(() => {
    if (saved === undefined) {
      delete process.env.MASTER_SEED
    } else {
      process.env.MASTER_SEED = saved
    }
  })

  test('keeps the seed that is configured and writes nothing', async () => {
    process.env.MASTER_SEED = '07'.repeat(32)
    const answer = await askForMasterSeed()
    expect(answer.seed).toEqual(new Uint8Array(32).fill(7))
    expect(answer.env).toEqual({})
  })

  test('refuses a configured seed that is not one, rather than replacing it', async () => {
    process.env.MASTER_SEED = 'not a seed'
    const failure = await askForMasterSeed().catch((error: unknown) => error)
    expect(failure).toBeInstanceOf(SetupError)
    expect((failure as SetupError).reason).toBe('master-seed-invalid')
  })
})
