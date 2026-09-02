/*
 * locales/<lang>.json  ->  po/<lang>/messages.po
 *
 * The importer's other half. po/ is the source now; locales/*.json is legacy's
 * key-based catalogue, kept so that a template arriving THERE can be brought over
 * here -- its strings by this tool, its markup by hand with tools/compare_pug.mjs
 * checking the result.
 *
 * Run it, read `git diff`: every string is accounted for.
 *
 * Three things change on the way:
 *
 *   the key      emails.accountActivation.pleaseClickLink   ->  the English text
 *   the params   "Hello {firstName} {lastName},"            ->  "Hello %1 %2,"
 *   line breaks  one string with \n in it                   ->  one message per line
 *
 * The last one is why a template can put a <br /> between two markers instead of
 * carrying a newline inside one. A marker that spans lines is fragile -- indent it
 * by accident and the lookup misses in silence, falling back to English -- and the
 * split pays for itself twice over: the trailing sentence of linkValidity and
 * linkValidityWithMinutes is the same in all ten languages, so it collapses into
 * one message instead of two.
 *
 * The English catalogue decides both. A key's msgid is its en.json value, and the
 * order the {names} appear in THAT string fixes which one is %1 -- so a language
 * that puts the surname first just writes "%2 %1" and the binding still holds.
 * That is why the positional form is the right one here and not a regression from
 * the named braces: it is what lets a translator reorder without touching code.
 * It is also what packages/frontend already does (`t.__('… %1 …', value)`).
 *
 *   node tools/json2po.mjs            MERGE: new messages in, existing ones kept
 *   node tools/json2po.mjs --check    verify the .po round-trips to the JSON
 *   node tools/json2po.mjs --overwrite   throw the .po away and rebuild from JSON
 *
 * Merge is the default, and that is the whole point once the .po files are the
 * source. A new template in legacy brings new keys with it; those come in, and a
 * translation somebody has since improved in the .po stays as it is. --overwrite
 * is the initial conversion and a way to lose work; it says so when you run it.
 */
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { readPo, writePo } from './po.mjs'

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const LOCALE_DIR = path.join(ROOT, 'locales')
const PO_DIR = path.join(ROOT, 'po')
const LOCALES = ['en', 'de', 'es', 'fr', 'nl', 'it', 'tr', 'ru', 'pt', 'el']
const SOURCE = 'en'

const flatten = (o, prefix = '') =>
  Object.entries(o).reduce((acc, [k, v]) => {
    if (v && typeof v === 'object') Object.assign(acc, flatten(v, `${prefix}${k}.`))
    else acc[`${prefix}${k}`] = v
    return acc
  }, {})

const read = (locale) =>
  flatten(JSON.parse(fs.readFileSync(path.join(LOCALE_DIR, `${locale}.json`), 'utf8')))

/** The {names} of a string, in order of first appearance -- 1-based, gettext.js style. */
const paramOrder = (s) => {
  const seen = []
  for (const m of s.matchAll(/\{([A-Za-z0-9_]+)\}/g)) if (!seen.includes(m[1])) seen.push(m[1])
  return seen
}

/** "Hello {firstName}" + [firstName] -> "Hello %1". Unknown names are a hard error. */
const toPositional = (s, order, where) =>
  s.replace(/\{([A-Za-z0-9_]+)\}/g, (m, name) => {
    const i = order.indexOf(name)
    if (i < 0) throw new Error(`${where}: {${name}} is not in the English string (params: ${order.join(', ') || 'none'})`)
    return `%${i + 1}`
  })

const source = read(SOURCE)
const keys = Object.keys(source)


/*
 * A message is one LINE of a catalogue string. The params are counted per line,
 * so `The link has a validity of {hours} hours.` keeps its %1 and the sentence
 * after it has none -- and a translation that moved {hours} to the other line
 * would fail in toPositional() rather than lose the value.
 *
 * A language must break a string into the same number of lines as English does,
 * which is checked below.
 */
const units = [] // { key, part, parts, order }
for (const k of keys) {
  const lines = source[k].split('\n')
  lines.forEach((line, i) =>
    units.push({ key: k, part: i, parts: lines.length, order: paramOrder(line) }),
  )
}

const partOf = (cat, u, locale) => {
  const lines = cat[u.key].split('\n')
  if (lines.length !== u.parts)
    throw new Error(
      `${locale}: ${u.key} has ${lines.length} line(s), English has ${u.parts} -- ` +
        'a split message must break the same way in every language',
    )
  return lines[u.part]
}

if (process.argv.includes('--check')) {
  // Round-trip: do the split, positional messages join back into exactly the
  // string the JSON holds? Compares against the catalogues, not against itself.
  let bad = 0
  for (const locale of LOCALES) {
    const cat = read(locale)
    for (const k of keys) {
      if (cat[k] === undefined) { console.error(`${locale}: missing ${k}`); bad++; continue }
      try {
        const back = units
          .filter((u) => u.key === k)
          .map((u) => {
            const positional = toPositional(partOf(cat, u, locale), u.order, `${locale}:${k}`)
            return positional.replace(/%(\d+)/g, (_, n) => `{${u.order[n - 1]}}`)
          })
          .join('\n')
        if (back !== cat[k]) { console.error(`${locale}: ${k} does not round-trip`); bad++ }
      } catch (e) { console.error(e.message); bad++ }
    }
  }
  const multi = keys.filter((k) => source[k].includes('\n')).length
  console.log(
    bad === 0
      ? `ok: ${keys.length} keys (${multi} of them split) -> ${units.length} messages, ` +
        `x ${LOCALES.length} locales, all round-trip`
      : `${bad} problem(s)`,
  )
  process.exit(bad === 0 ? 0 : 1)
}

// A msgid is unique in a .po -- msgfmt rejects a duplicate. The ten English
// strings that two keys share are therefore one entry carrying both keys, which
// the conflict check above has already shown to be safe.
const entries = []
const seenMsgid = new Map()
for (const u of units) {
  const label = u.parts > 1 ? `${u.key} [${u.part + 1}/${u.parts}]` : u.key
  const msgid = toPositional(source[u.key].split('\n')[u.part], u.order, `en:${u.key}`)
  const at = seenMsgid.get(msgid)
  if (at !== undefined) { entries[at].keys.push(label); entries[at].also.push(u); continue }
  seenMsgid.set(msgid, entries.length)
  entries.push({ msgid, keys: [label], unit: u, also: [] })
}

/*
 * The one thing that could make this conversion lossy: two places share an
 * English message but not its translation. gettext has one msgstr per msgid, so
 * that case needs msgctxt -- and it is worth failing loudly over rather than
 * silently giving both places whichever translation came first.
 */
const conflicts = []
for (const e of entries) {
  if (!e.also.length) continue
  for (const locale of LOCALES) {
    const cat = read(locale)
    const seen = new Set([e.unit, ...e.also].map((u) => partOf(cat, u, locale)))
    if (seen.size > 1) {
      conflicts.push(`  ${JSON.stringify(e.msgid)}\n    ${e.keys.join(', ')}\n    differs in ${locale}: ${[...seen].map((s) => JSON.stringify(s)).join(' vs ')}`)
      break
    }
  }
}
if (conflicts.length) {
  console.error('Places share an English message but not a translation -- these need msgctxt:\n')
  console.error(`${conflicts.join('\n')}\n`)
  process.exit(1)
}

const OVERWRITE = process.argv.includes('--overwrite')
let totalNew = 0
let totalKept = 0
let totalStale = 0

for (const locale of LOCALES) {
  const cat = read(locale)
  const file = path.join(PO_DIR, locale, 'messages.po')
  const existing = !OVERWRITE && fs.existsSync(file) ? readPo(file) : { byId: new Map() }

  const out = entries.map((e) => {
    const fromJson = toPositional(partOf(cat, e.unit, locale), e.unit.order, `${locale}:${e.unit.key}`)
    const had = existing.byId.get(e.msgid)
    // English is the source language: msgstr stays empty, gettext falls back to
    // the msgid. That is what makes a missing translation degrade to English
    // instead of to nothing.
    if (locale === SOURCE) return { comments: e.keys.map((k) => `#. ${k}`), msgid: e.msgid, msgstr: '' }
    if (had === undefined) totalNew++
    else totalKept++
    return {
      comments: e.keys.map((k) => `#. ${k}`),
      msgid: e.msgid,
      // What is already in the .po wins: it may have been corrected there since.
      msgstr: had ?? fromJson,
    }
  })

  const known = new Set(entries.map((e) => e.msgid))
  for (const id of existing.byId.keys()) if (!known.has(id)) totalStale++

  fs.mkdirSync(path.dirname(file), { recursive: true })
  writePo(file, locale, out)
}

const langs = LOCALES.length - 1 // English carries no translations
console.log(
  OVERWRITE
    ? `overwritten: ${entries.length} messages x ${LOCALES.length} locales, from locales/*.json`
    : `merged: ${totalNew / langs || 0} new, ${totalKept / langs || 0} kept per language` +
      `, ${entries.length} messages x ${LOCALES.length} locales`,
)
if (totalStale)
  console.log(
    `${totalStale / langs} message(s) are in the .po but no longer in locales/*.json` +
      ' -- dropped, which is what a removed template looks like.',
  )
