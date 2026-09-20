import * as v from 'valibot'
import { contractValueSchema, loadVectors } from './vectors.ts'

/** The shape of `contracts/test-vectors/master-seed.json`: three kinds of vector in one file. */

const hex = (bytes: number) => v.pipe(v.string(), v.regex(new RegExp(`^[0-9a-f]{${bytes * 2}}$`)))

const common = {
  id: v.string(),
  why: v.pipe(v.string(), v.nonEmpty('every vector says what it is load bearing for')),
}

export const masterSeedVectorSchema = v.variant('kind', [
  v.object({
    ...common,
    kind: v.literal('parse'),
    text: v.string(),
    expect: v.object({ accepted: v.boolean() }),
  }),
  v.object({
    ...common,
    kind: v.literal('derive'),
    masterSeed: hex(32),
    expect: v.object({ dhtNodeSeed: hex(32), dhtNodeKey: hex(32) }),
  }),
  v.object({
    ...common,
    kind: v.literal('delegate'),
    communitySeed: hex(32),
    masterSeed: hex(32),
    /** unix milliseconds as a decimal string, 0 for never. */
    expiresMs: v.pipe(v.string(), v.regex(/^\d+$/u)),
    expect: v.object({ communityKey: hex(32), dhtNodeKey: hex(32), delegation: hex(136) }),
  }),
])

export type MasterSeedVector = v.InferOutput<typeof masterSeedVectorSchema>

export function loadMasterSeedVectors(): {
  readonly pathDht: number
  readonly seedBytes: number
  readonly vectors: MasterSeedVector[]
} {
  const { file, vectors } = loadVectors('master-seed', masterSeedVectorSchema)
  const rules = v.parse(
    v.object({ pathDht: contractValueSchema, seedBytes: contractValueSchema }),
    file.rules,
  )
  return { pathDht: Number(rules.pathDht.value), seedBytes: Number(rules.seedBytes.value), vectors }
}
