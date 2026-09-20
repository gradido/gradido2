import { describe, expect, test } from 'bun:test'
import { isPublicUrl } from '@gradido/service-core'
import { loadPublicUrlVectors } from './public-url.vectors.ts'

/**
 * `contracts/test-vectors/public-url.json`, run against the TypeScript path. The other half is
 * `fast-servers/tests/contract/test_public_url_contract.cpp`.
 */
describe('contract vectors: public-url', () => {
  for (const vector of loadPublicUrlVectors()) {
    test(vector.id, () => {
      expect(isPublicUrl(vector.url)).toBe(vector.expect.public)
    })
  }
})
