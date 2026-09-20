import * as v from 'valibot'
import { loadVectors } from './vectors.ts'

/** The shape of `contracts/test-vectors/public-url.json`. */
export const publicUrlVectorSchema = v.object({
  id: v.string(),
  why: v.pipe(v.string(), v.nonEmpty('every vector says what it is load bearing for')),
  url: v.string(),
  expect: v.object({ public: v.boolean() }),
})

export type PublicUrlVector = v.InferOutput<typeof publicUrlVectorSchema>

export function loadPublicUrlVectors(): PublicUrlVector[] {
  return loadVectors('public-url', publicUrlVectorSchema).vectors
}
