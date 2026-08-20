/**
 * Compile every `src/locales/<lang>/messages.po` into `public/locales/<lang>/messages.json`.
 *
 * Catalogs are served as files rather than bundled, so a translation fix does not need
 * a rebuild of the app. The generated JSON is therefore build output, not source.
 */
import { mkdirSync, readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import po2json from 'po2json'

const root = join(dirname(fileURLToPath(import.meta.url)), '..')
const localeDir = join(root, 'src', 'locales')

const languages = readdirSync(localeDir).filter((entry) =>
  statSync(join(localeDir, entry)).isDirectory(),
)

const DEFAULT_PLURAL_FORMS = 'nplurals=2; plural=(n != 1);'

/** `mf` drops the PO header, but gettext.js needs it to know the plural rule. */
const pluralForms = (po: string): string =>
  po.match(/"Plural-Forms: (.*?)\\n"/)?.[1] ?? DEFAULT_PLURAL_FORMS

for (const language of languages) {
  const source = join(localeDir, language, 'messages.po')
  // `mf` is the shape gettext.js `loadJSON` expects.
  const catalog = po2json.parseFileSync(source, { format: 'mf' })
  catalog[''] = { language, 'plural-forms': pluralForms(readFileSync(source, 'utf8')) }
  const target = join(root, 'public', 'locales', language)
  mkdirSync(target, { recursive: true })
  writeFileSync(join(target, 'messages.json'), JSON.stringify(catalog))
  console.log(`locale: ${language} -> public/locales/${language}/messages.json`)
}
