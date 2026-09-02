/*
 * What turns the placeholders in the .mjml sources into readable text while you
 * are designing -- and nothing else. This file exists for the editor's live
 * preview; the build does the same two substitutions in its own extractor, with
 * {{v:...}} becoming a sentinel rather than a sample value.
 *
 * MJML applies `preprocessors` to the raw XML before parsing, and it does so on
 * every recursive parse -- so this reaches inside templates/includes/ too.
 *
 *   {{t:Activate account}}          -> the translation, or the English as written
 *   {{t:Hello %1 %2,|first,last}}   -> same, with %1/%2 filled from the slots named
 *   {{v:activationLink}}            -> a sample value from .preview-values.json
 *
 * The message IS the English text, the way packages/frontend writes `t.__('Sign
 * in')`. Two things follow from that and both are the reason for the change:
 * a template reads like an email with no tooling in the way, and a missing
 * translation degrades to English rather than to a key or a blank.
 *
 * Catalogues are po/<lang>/messages.po. English carries empty msgstr throughout,
 * which is the gettext idiom for "the source is the translation".
 *
 * No message spans lines: json2po.mjs makes one message per line, so a marker is
 * always a single line of the template and a stray indent cannot break a lookup.
 * A line break in the mail is a <br /> between two markers, written where it is
 * visible rather than hidden inside a catalogue string.
 *
 * The language is .preview-locale (gitignored, yours alone), or MJML_LOCALE for a
 * single run. No language in po/ is more than 2% longer than German, so pick
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
 *     rendering, with raw {{t:...}} where the text should be. If you see markers
 *     instead of sentences, look at the console, not at the template.
 * ─────────────────────────────────────────────────────────────────────────────
 */
const fs = require('fs')
const path = require('path')

const readLocale = () => {
  if (process.env.MJML_LOCALE) return process.env.MJML_LOCALE
  try {
    return fs.readFileSync(path.join(__dirname, '.preview-locale'), 'utf8').trim() || 'en'
  } catch {
    return 'en' // fresh checkout: .preview-locale is gitignored
  }
}

/**
 * Enough of a .po reader for a catalogue msgfmt accepts: `msgid`/`msgstr` with
 * continuation lines, comments skipped. No plurals -- none of these messages has
 * one, and a silent wrong answer would be worse than the missing feature.
 */
const readPo = (file) => {
  const unquote = (line) =>
    line
      .slice(line.indexOf('"') + 1, line.lastIndexOf('"'))
      .replace(/\\(n|t|"|\\)/g, (_, c) => ({ n: '\n', t: '\t', '"': '"', '\\': '\\' })[c])

  const catalogue = {}
  let field = null
  let id = ''
  let str = ''
  const flush = () => {
    if (id) catalogue[id] = str || id // empty msgstr: the source is the translation
    id = ''
    str = ''
  }
  for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
    if (line.startsWith('#') || line.trim() === '') {
      if (line.trim() === '') flush()
      continue
    }
    if (line.startsWith('msgid ')) { flush(); field = 'id'; id = unquote(line); continue }
    if (line.startsWith('msgstr ')) { field = 'str'; str = unquote(line); continue }
    if (line.trimStart().startsWith('"')) {
      if (field === 'id') id += unquote(line)
      else str += unquote(line)
    }
  }
  flush()
  return catalogue
}

// The four characters pug escapes, which is what the generated C escapes too.
const escape = (s) =>
  String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c])

const locale = readLocale()
const catalogue = readPo(path.join(__dirname, 'po', locale, 'messages.po'))
const values = JSON.parse(fs.readFileSync(path.join(__dirname, '.preview-values.json'), 'utf8'))
// `locale` is not a caller's value: the renderer fills it from the locale it was
// called with. In the preview that is simply the locale being previewed.
values.locale = locale

const sample = (name) => (name in values ? escape(values[name]) : `[[no sample value: ${name}]]`)

// msgid and the argument list are free of { } |, which json2po.mjs guarantees --
// so the markers need no escaping of their own.
const substitute = (xml) =>
  xml
    .replace(/\{\{v:([A-Za-z0-9_]+)\}\}/g, (_, name) => sample(name))
    .replace(/\{\{t:([^|{}]+)(?:\|([^{}]*))?\}\}/g, (_, msgid, args) => {
      const slots = (args || '').split(',').map((s) => s.trim()).filter(Boolean)
      // An unknown msgid is not an error: gettext falls back to the source text,
      // and here that is exactly what is written in the template.
      const text = catalogue[msgid] ?? msgid
      return escape(text)
        .replace(/%(\d+)/g, (m, n) => (slots[n - 1] ? sample(slots[n - 1]) : m))
    })

module.exports = { preprocessors: [substitute] }
