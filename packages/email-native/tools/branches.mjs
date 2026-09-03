/*
 * The branch markers, on their own so they can be tested without running an
 * extraction.
 *
 *   <!--@if x-->  A  <!--@elif y-->  B  <!--@else-->  C  <!--@endif-->
 *
 * They are HTML comments on their own line, which buys two things: MJML never has
 * to understand them (they are gone before it parses), and the drift check is an
 * exact count over a syntax this repository owns rather than a regex over somebody
 * else's keywords.
 *
 * A case index counts the alternatives in source order: 0 is the @if, 1 the first
 * @elif or the @else, and so on. An @if WITHOUT an @else still has a last case --
 * the empty one, which is what "no logo" ships as.
 */

export const MARKER = /^[ \t]*<!--@(if|elif|else|endif)(?:\s+([A-Za-z0-9_]+))?-->[ \t]*$/

/**
 * Walk the include graph in document order and collect the branches.
 *
 * Order matters: it is the order the variant index counts in, and it has to match
 * the order tools/manifest.mjs lists the conditions in, or the two number the same
 * documents differently.
 *
 * @param {string} file
 * @param {{ readFile: (f: string) => string, resolve: (from: string, to: string) => string }} io
 * @returns {{ id: string, cases: number }[]}
 */
export function scanBranches(file, io, seen = new Set()) {
  if (seen.has(file)) return []
  seen.add(file)
  const out = []
  const stack = []
  for (const line of io.readFile(file).split('\n')) {
    const m = MARKER.exec(line)
    if (m) {
      const [, kind, id] = m
      if (kind === 'if') {
        if (!id) throw new Error(`${file}: <!--@if--> without a variable`)
        const b = { id, cases: 1, hasElse: false }
        out.push(b)
        stack.push(b)
      } else if (kind === 'endif') {
        const b = stack.pop()
        if (!b) throw new Error(`${file}: <!--@endif--> without an open <!--@if-->`)
        if (!b.hasElse) b.cases++ // the implicit empty case
      } else {
        const b = stack.at(-1)
        if (!b) throw new Error(`${file}: <!--@${kind}--> without an open <!--@if-->`)
        if (b.hasElse) throw new Error(`${file}: <!--@${kind}--> after <!--@else--> in '${b.id}'`)
        b.cases++
        if (kind === 'else') b.hasElse = true
      }
      continue
    }
    const inc = /<mj-include\s+path="([^"]+)"/.exec(line)
    if (inc) out.push(...scanBranches(io.resolve(file, inc[1]), io, seen))
  }
  if (stack.length) throw new Error(`${file}: ${stack.length} unclosed <!--@if-->`)
  return out.map(({ id, cases }) => ({ id, cases }))
}

/**
 * Keep one case of every branch and drop the rest. Text level, before MJML parses
 * -- so the markers never have to be valid MJML, and a dropped branch costs
 * nothing downstream. Marker lines are never emitted.
 *
 * @param {string} xml
 * @param {Record<string, number>} chosen  branch id -> case index, default 0
 */
export function selectBranches(xml, chosen) {
  const out = []
  const stack = [] // one frame per open @if: { wanted, at }
  for (const line of xml.split('\n')) {
    const m = MARKER.exec(line)
    if (!m) {
      // A line survives only if every enclosing branch is currently in the case
      // that was chosen for it -- which is also what makes nesting work.
      if (stack.every((f) => f.at === f.wanted)) out.push(line)
      continue
    }
    const [, kind, id] = m
    if (kind === 'if') stack.push({ wanted: chosen[id] ?? 0, at: 0 })
    else if (kind === 'endif') stack.pop()
    else stack.at(-1).at++
  }
  return out.join('\n')
}
