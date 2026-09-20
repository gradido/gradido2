import { resolveSecrets } from '@gradido/service-core'
import { newMasterSeed, parseMasterSeed } from '@gradido/shared/crypto'
import { askSecret, say } from './prompt'
import { SetupError } from './requireHomeCommunity'

/** The master seed once `setup` has settled it. */
export type MasterSeedAnswer = {
  readonly seed: Uint8Array
  /** `MASTER_SEED` for the `.env`, or nothing when the seed stays where it already is. */
  readonly env: Readonly<Record<string, string>>
}

/**
 * The instance's master seed: the one it has, or a new one -- `contracts/secrets.json`,
 * `MASTER_SEED`. `fast-servers/backend/src/setup.c` asks the same.
 *
 * A seed that exists is never replaced, whichever source it comes from: every identity derived
 * from it would change with it, and the network would meet a stranger under the old name.
 */
export async function askForMasterSeed(): Promise<MasterSeedAnswer> {
  const current = currentMasterSeed()
  if (current !== undefined) {
    const seed = parseMasterSeed(current)
    if (seed === undefined) {
      throw new SetupError(
        'master-seed-invalid',
        'MASTER_SEED is set but is not 64 hex digits. It is the root of this instance’s keys, so setup does not replace it: correct it, or remove it to have a new one made.',
      )
    }
    return { seed, env: {} }
  }

  say(
    '',
    'This instance needs a master seed: the root of the keys it derives, its peer',
    'network identity first. The system’s randomness makes it; whatever you type',
    'now is mixed in as well.',
    '',
  )
  const seed = newMasterSeed(await askSecret('Type some random keys', '', 'optional'))
  return { seed, env: { MASTER_SEED: Buffer.from(seed).toString('hex') } }
}

/** The configured seed as text, or undefined; a named source that cannot be read is refused. */
function currentMasterSeed(): string | undefined {
  try {
    return resolveSecrets(process.env).MASTER_SEED
  } catch (error) {
    throw new SetupError(
      'master-seed-invalid',
      `MASTER_SEED could not be read from the source that names it: ${error instanceof Error ? error.message : String(error)}`,
    )
  }
}
