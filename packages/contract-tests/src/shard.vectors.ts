import * as v from 'valibot'
import { contractValueSchema, loadVectors } from './vectors.ts'

/** The shape of `contracts/test-vectors/shard.json`: two kinds of vector in one file. */

const hex32 = v.pipe(v.string(), v.regex(/^[0-9a-f]{64}$/u))
const decimal = v.pipe(v.string(), v.regex(/^\d+$/u))
const common = {
  id: v.string(),
  why: v.pipe(v.string(), v.nonEmpty('every vector says what it is load bearing for')),
}

export const shardVectorSchema = v.variant('kind', [
  v.object({
    ...common,
    kind: v.literal('community'),
    communityKey: hex32,
    expect: v.object({ shard: decimal, shardTopicKey: hex32, communityTopicKey: hex32 }),
  }),
  v.object({
    ...common,
    kind: v.literal('topic'),
    shard: decimal,
    expect: v.object({ shardTopicKey: hex32 }),
  }),
])

export type ShardVector = v.InferOutput<typeof shardVectorSchema>

export function loadShardVectors(): {
  readonly shardCount: number
  readonly vectors: ShardVector[]
} {
  const { file, vectors } = loadVectors('shard', shardVectorSchema)
  const rules = v.parse(v.object({ shardCount: contractValueSchema }), file.rules)
  return { shardCount: Number(rules.shardCount.value), vectors }
}
