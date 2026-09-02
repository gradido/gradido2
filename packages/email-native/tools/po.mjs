/*
 * Reading and writing gettext .po, in as much of the format as these catalogues
 * use: msgid/msgstr with continuation lines, `#.` comments, no plurals. None of
 * the messages has a plural, and a silently wrong answer would be worse than the
 * missing feature -- so there is no half-support for one.
 *
 * Two readers exist on purpose. This one is ESM and shared by the tools; the one
 * in .mjmlconfig.js is CommonJS because MJML require()s that file, and it cannot
 * import from here.
 */
import fs from 'node:fs'

const unquote = (line) =>
  line
    .slice(line.indexOf('"') + 1, line.lastIndexOf('"'))
    .replace(/\\(n|t|"|\\)/g, (_, c) => ({ n: '\n', t: '\t', '"': '"', '\\': '\\' })[c])

/**
 * @returns {{ entries: {msgid: string, msgstr: string, comments: string[]}[],
 *             byId: Map<string, string> }}  header entry excluded
 */
export function readPo(file) {
  const entries = []
  let comments = []
  let field = null
  let id = ''
  let str = ''
  const flush = () => {
    if (id) entries.push({ msgid: id, msgstr: str, comments })
    comments = []
    id = ''
    str = ''
  }
  for (const line of fs.readFileSync(file, 'utf8').split('\n')) {
    if (line.startsWith('#')) { comments.push(line); continue }
    if (line.trim() === '') { flush(); continue }
    if (line.startsWith('msgid ')) { flush(); field = 'id'; id = unquote(line); continue }
    if (line.startsWith('msgstr ')) { field = 'str'; str = unquote(line); continue }
    if (line.trimStart().startsWith('"')) {
      if (field === 'id') id += unquote(line)
      else str += unquote(line)
    }
  }
  flush()
  return { entries, byId: new Map(entries.map((e) => [e.msgid, e.msgstr])) }
}

/** An empty msgstr is gettext for "the source is the translation". */
export const translationOf = (byId, msgid) => byId.get(msgid) || msgid

export const poString = (s) => {
  const escaped = s.replace(/\\/g, '\\\\').replace(/"/g, '\\"').replace(/\t/g, '\\t')
  if (!escaped.includes('\n')) return `"${escaped}"`
  const parts = escaped.split('\n')
  const lines = parts.slice(0, -1).map((p) => `"${p}\\n"`)
  if (parts.at(-1) !== '') lines.push(`"${parts.at(-1)}"`)
  return `""\n${lines.join('\n')}`
}

export const header = (locale, project = 'gradido2-email') =>
  [
    'msgid ""',
    'msgstr ""',
    `"Project-Id-Version: ${project}\\n"`,
    '"MIME-Version: 1.0\\n"',
    '"Content-Type: text/plain; charset=UTF-8\\n"',
    '"Content-Transfer-Encoding: 8bit\\n"',
    `"Language: ${locale}\\n"`,
    '"Plural-Forms: nplurals=2; plural=(n != 1);\\n"',
    '',
  ].join('\n')

/** @param {{comments: string[], msgid: string, msgstr: string}[]} entries */
export const writePo = (file, locale, entries) =>
  fs.writeFileSync(
    file,
    `${header(locale)}\n${entries
      .map((e) => [...e.comments, `msgid ${poString(e.msgid)}`, `msgstr ${poString(e.msgstr)}`].join('\n'))
      .join('\n\n')}\n`,
  )
