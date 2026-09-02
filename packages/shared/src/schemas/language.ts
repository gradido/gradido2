import * as v from 'valibot'

/**
 * The languages a member can be written to in — `contracts/types/Language.json`.
 *
 * This is not the set of locales the frontend has catalogs for. That set is smaller, it
 * changes when someone finishes a translation, and it concerns one implementation only;
 * see `contracts/AGENTS.md`, working rule 3. A member may well be addressed in a language
 * the interface does not speak yet.
 */
export const LANGUAGES = ['de', 'en', 'es', 'fr', 'nl', 'it', 'tr', 'ru', 'pt', 'el'] as const

export type Language = (typeof LANGUAGES)[number]

export const DEFAULT_LANGUAGE: Language = 'de'

export function isLanguage(value: unknown): value is Language {
  return typeof value === 'string' && (LANGUAGES as readonly string[]).includes(value)
}

/**
 * An unknown or absent language becomes the default rather than a rejected request.
 *
 * That is `unknownValuePolicy: "ignore_and_warn"` in the contract, and it is deliberate:
 * the value arrives from the browser's locale, which nobody typed and nobody can correct
 * from the form. A registration that fails because a visitor's browser says `de-AT` would
 * be a bug in us, not in them.
 */
export const languageSchema = v.optional(
  v.pipe(
    v.unknown(),
    v.transform((value: unknown): Language => (isLanguage(value) ? value : DEFAULT_LANGUAGE)),
  ),
  DEFAULT_LANGUAGE,
)
