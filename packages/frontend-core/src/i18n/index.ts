import i18n from 'gettext.js'

/**
 * The translator. Source strings are English, so `t.__('Sign in')` renders correctly
 * even before a catalog is loaded — a failed catalog fetch degrades to English rather
 * than to empty labels.
 *
 * Exported as a value instead of the `globalThis.t` the inspector uses, so that call
 * sites are typed and a missing i18n init is a compile error rather than a blank page.
 */
export const t = i18n()

export const SUPPORTED_LOCALES = ['de', 'en'] as const
export type Locale = (typeof SUPPORTED_LOCALES)[number]
export const DEFAULT_LOCALE: Locale = 'en'
export const LOCALE_STORAGE_KEY = 'gradido-language'

/** Autonyms — a language picker shows every language in its own words, not in the current one. */
export const LOCALE_NAMES: Record<Locale, string> = {
  de: 'Deutsch',
  en: 'English',
}

export function isLocale(value: string | null | undefined): value is Locale {
  return value !== null && value !== undefined && SUPPORTED_LOCALES.includes(value as Locale)
}

/**
 * Pick a supported locale from a stored choice, a browser tag or nothing.
 * Region subtags are dropped: `de-AT` is served by the `de` catalog.
 */
export function resolveLocale(...preferred: (string | null | undefined)[]): Locale {
  for (const candidate of preferred) {
    if (!candidate) {
      continue
    }
    const language = candidate.split('-')[0]
    if (isLocale(language)) {
      return language
    }
  }
  return DEFAULT_LOCALE
}

let current: Locale = DEFAULT_LOCALE

/** The locale in effect right now. */
export function currentLocale(): Locale {
  return current
}

/** The locale to start with: an earlier deliberate choice wins over the browser's. */
export function storedLocale(): Locale {
  return resolveLocale(localStorage.getItem(LOCALE_STORAGE_KEY), navigator.language)
}

/**
 * Load a catalog and switch to it. Catalogs are fetched rather than bundled so that
 * adding a language does not rebuild the app.
 */
export async function setLocale(locale: Locale, basePath = '/'): Promise<void> {
  current = locale
  const base = basePath.endsWith('/') ? basePath : `${basePath}/`
  try {
    const response = await fetch(`${base}locales/${locale}/messages.json`)
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`)
    }
    t.loadJSON(await response.json(), 'messages')
  } catch (error) {
    // English source strings stay readable, so a missing catalog is not fatal.
    console.warn(`no message catalog for '${locale}', falling back to source strings`, error)
  }
  t.setLocale(locale)
  localStorage.setItem(LOCALE_STORAGE_KEY, locale)
  document.documentElement.setAttribute('lang', locale)
}

/**
 * Translate a validation message coming out of a valibot schema.
 *
 * The schemas live in `shared` and carry English messages, which `gettext-extract`
 * cannot see because they are not `t.__` call sites. Listing them here once makes them
 * extractable and keeps the schemas free of any dependency on the frontend.
 */
export function translateIssue(message: string): string {
  switch (message) {
    case 'This field is required':
      return t.__('This field is required')
    case 'Please enter a valid email address':
      return t.__('Please enter a valid email address')
    case 'This email address is too long':
      return t.__('This email address is too long')
    case 'An email address cannot contain spaces':
      return t.__('An email address cannot contain spaces')
    default:
      return t.__(message)
  }
}
