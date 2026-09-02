import { describe, expect, test } from 'bun:test'
import * as v from 'valibot'
import { DEFAULT_LANGUAGE, isLanguage, LANGUAGES, languageSchema } from './language'

describe('languageSchema', () => {
  test('keeps every language the contract lists', () => {
    for (const language of LANGUAGES) {
      expect(v.parse(languageSchema, language)).toBe(language)
    }
  })

  // The value arrives from the browser's locale, which nobody typed and nobody can correct
  // from the form. Rejecting it would fail a registration over something the visitor has no
  // way to fix — contracts/types/Language.json calls this ignore_and_warn.
  test.each([
    undefined,
    null,
    '',
    'de-AT',
    'klingon',
    42,
    {},
  ])('falls back to the default rather than refusing: %p', (input) => {
    expect(v.parse(languageSchema, input)).toBe(DEFAULT_LANGUAGE)
  })

  test('the default is one of the languages', () => {
    expect(isLanguage(DEFAULT_LANGUAGE)).toBe(true)
  })
})
