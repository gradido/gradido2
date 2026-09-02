/*
 * What turns the placeholders in the .mjml sources into readable text while you
 * are designing -- and nothing else. This file exists for the editor's live
 * preview; the build does the same two substitutions in its own extractor, with
 * {{v:...}} becoming a sentinel rather than a sample value.
 *
 * MJML applies `preprocessors` to the raw XML before parsing, and it does so on
 * every recursive parse -- so this reaches inside includes/ too.
 *
 *   {{t:emails.accountActivation.title}}   -> the catalogue string, HTML-escaped
 *   {{v:firstName}}                        -> a sample value from .preview-values.json
 *   {firstName} inside a catalogue string  -> the same sample value
 *   a missing key                          -> [[missing: key]], visible in place
 *
 * The language is .preview-locale (gitignored, yours alone), or MJML_LOCALE for a
 * single run. No language in locales/ is more than 2% longer than German, so pick
 * whichever you read best -- what breaks a layout here is a long slot value, not a
 * long translation.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * THREE THINGS THAT FAIL QUIETLY, all of them about how MJML finds this file:
 *
 *   - mjml.mjmlConfigPath must name THIS FILE, not the directory. Given a
 *     directory, MJML looks for `.mjmlconfig` and parses it as JSON -- and JSON
 *     cannot hold a function, so you get no preprocessor and no complaint.
 *   - The name is checked against /^\.mjmlconfig(\.js)?$/, so `.mjmlconfig.cjs`
 *     is not recognised. It must still be CommonJS, because MJML require()s it;
 *     that is the default here since the package declares no "type": "module".
 *   - An error in this file is logged and then ignored. The preview keeps
 *     rendering, with raw {{t:...}} where the text should be. If you see keys
 *     instead of sentences, look at the console, not at the template.
 * ─────────────────────────────────────────────────────────────────────────────
 */
const fs = require('fs')
const path = require('path')

const readLocale = () => {
  if (process.env.MJML_LOCALE) return process.env.MJML_LOCALE
  try {
    return fs.readFileSync(path.join(__dirname, '.preview-locale'), 'utf8').trim() || 'de'
  } catch {
    return 'de' // fresh checkout: .preview-locale is gitignored
  }
}

const flatten = (o, prefix = '') =>
  Object.entries(o).reduce((acc, [k, v]) => {
    if (v && typeof v === 'object') Object.assign(acc, flatten(v, `${prefix}${k}.`))
    else acc[`${prefix}${k}`] = v
    return acc
  }, {})

// The four characters pug escapes, which is what the generated C escapes too.
const escape = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c])

const locale = readLocale()
const catalogue = flatten(
  JSON.parse(fs.readFileSync(path.join(__dirname, 'locales', `${locale}.json`), 'utf8')),
)
const values = JSON.parse(fs.readFileSync(path.join(__dirname, '.preview-values.json'), 'utf8'))

// A catalogue string carries {name} placeholders of its own -- locales are
// configured with mustacheConfig.tags = ['{','}'] in legacy. Same source of
// values, so `Hallo {firstName} {lastName},` fills from .preview-values.json.
const fillParams = (s) => s.replace(/\{([A-Za-z0-9_]+)\}/g, (m, k) => (k in values ? escape(values[k]) : m))

const substitute = (xml) =>
  xml
    .replace(/\{\{v:([A-Za-z0-9_]+)\}\}/g, (_, name) =>
      name in values ? escape(values[name]) : `[[no sample value: ${name}]]`,
    )
    .replace(/\{\{t:([A-Za-z0-9_.]+)\}\}/g, (_, key) =>
      catalogue[key] === undefined
        ? `[[missing: ${key}]]`
        : fillParams(escape(catalogue[key])).replace(/\n/g, '<br />'),
    )

module.exports = { preprocessors: [substitute] }
