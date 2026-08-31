// <out>/ir.json  ->  <out>/service_core/email/templates.h + <out>/templates.c
//
// The header sits under service_core/email/ so that the include spelling is the one
// fast-servers uses -- the two files are copied into service-core/email/ after the
// build, see scripts/sync-fast-servers.ts.
//
// Every literal of every rendering goes into ONE byte pool. A literal is not
// stored whole: it is assembled greedily from pool pieces that are already
// there (LZ77-style, long matches only). That way the 18 KB of CSS sits in the
// binary exactly once even though it appears in 170 documents -- and at runtime
// it is still a plain memcpy, nothing is decompressed.
import fs from 'fs'
import path from 'path'
import { TEMPLATE_ROOT, OUT_DIR, IR_PATH, ASSETS } from './manifest.mjs'

const ir = JSON.parse(fs.readFileSync(IR_PATH, 'utf8'))

const MIN_MATCH = 48 // shorter matches do not pay for their 8-byte op
const ANCHOR = 24

// latin1 == one char per byte, so string offsets are byte offsets
const toBytes = (s) => Buffer.from(s, 'utf8').toString('latin1')

class Pool {
  constructor() {
    this.s = ''
    this.idx = new Map()
    this.indexed = 0
  }
  _reindex() {
    const end = this.s.length - ANCHOR
    for (let i = this.indexed; i <= end; i++) {
      const k = this.s.substr(i, ANCHOR)
      let a = this.idx.get(k)
      if (!a) this.idx.set(k, (a = []))
      if (a.length < 4) a.push(i)
    }
    this.indexed = Math.max(this.indexed, end + 1)
  }
  _append(chunk) {
    const off = this.s.length
    this.s += chunk
    this._reindex()
    return { off, len: chunk.length }
  }
  // literal -> list of pool references
  encode(lit) {
    const ops = []
    let i = 0
    let pending = ''
    while (i < lit.length) {
      let bestLen = 0
      let bestOff = 0
      if (lit.length - i >= MIN_MATCH) {
        for (const p of this.idx.get(lit.substr(i, ANCHOR)) ?? []) {
          let l = ANCHOR
          while (i + l < lit.length && p + l < this.s.length && this.s[p + l] === lit[i + l]) l++
          if (l > bestLen) {
            bestLen = l
            bestOff = p
          }
        }
      }
      if (bestLen >= MIN_MATCH) {
        if (pending) {
          ops.push(this._append(pending))
          pending = ''
        }
        ops.push({ off: bestOff, len: bestLen })
        i += bestLen
      } else {
        pending += lit[i++]
      }
    }
    if (pending) ops.push(this._append(pending))
    return ops
  }
}

// --------------------------------------------------- collect the literals
const litIds = new Map() // literal(bytes) -> id
const lits = []
let rawTotal = 0
for (const t of ir.templates)
  for (const kind of ['html', 'subject', 'text'])
    for (const byLocale of t.renders[kind])
      for (const ops of byLocale)
        for (const op of ops)
          if (op.t === 'lit') {
            const b = toBytes(op.s)
            rawTotal += b.length
            if (!litIds.has(b)) {
              litIds.set(b, lits.length)
              lits.push(b)
            }
          }

// Largest literals first: that puts the shared body (CSS, header, footer) into
// the pool in one piece, and everything after it merely references that.
const pool = new Pool()
const litOps = new Array(lits.length)
for (const id of lits.map((_, i) => i).sort((a, b) => lits[b].length - lits[a].length))
  litOps[id] = pool.encode(lits[id])

// --------------------------------------------------------- build programs
const OPS = [] // global op table
const SLOT_HTML = 0xffffffff
const SLOT_RAW = 0xfffffffe

function emitProgram(ops, slotIndex) {
  const start = OPS.length
  let staticBytes = 0
  let slotRefs = 0
  for (const op of ops) {
    if (op.t === 'lit') {
      for (const r of litOps[litIds.get(toBytes(op.s))]) OPS.push([r.off, r.len])
      staticBytes += toBytes(op.s).length
    } else {
      OPS.push([slotIndex.get(op.name), op.esc === 'raw' ? SLOT_RAW : SLOT_HTML])
      slotRefs++
    }
  }
  return { prog: [start, OPS.length - start], staticBytes, slotRefs }
}

const camelToSnake = (s) =>
  s.replace(/([a-z0-9])([A-Z])/g, '$1_$2').replace(/([A-Z]+)([A-Z][a-z])/g, '$1_$2').toLowerCase()
const UP = (s) => camelToSnake(s).toUpperCase()

const templates = ir.templates.map((t) => {
  // Index 0 is reserved for 'locale' -- the renderer fills that one itself.
  const slotOrder = ['locale', ...t.slots]
  const slotIndex = new Map(slotOrder.map((n, i) => [n, i]))
  const progs = []
  const max = { html: 0, subject: 0, text: 0, refs: 0 }
  for (let li = 0; li < ir.locales.length; li++)
    for (let ci = 0; ci < t.combos.length; ci++)
      /* The order is the one ge_emit_* indexes with: html, subject, text. */
      for (const kind of ['html', 'subject', 'text']) {
        const r = emitProgram(t.renders[kind][li][ci], slotIndex)
        progs.push(r.prog)
        max[kind] = Math.max(max[kind], r.staticBytes)
        max.refs = Math.max(max.refs, r.slotRefs)
      }
  return { ...t, slotOrder, progs, max, progBase: null }
})

let progBase = 0
const PROGS = []
for (const t of templates) {
  t.progBase = progBase
  for (const p of t.progs) PROGS.push(p)
  progBase += t.progs.length
}

// -------------------------------------------------------------- C source
const poolBytes = Buffer.from(pool.s, 'latin1')

function cStringLiteral(buf) {
  const out = []
  let line = ''
  for (const b of buf) {
    const c = String.fromCharCode(b)
    // Three-digit octal escapes: no following digit can extend the escape.
    if (b === 0x22 || b === 0x5c || b === 0x3f) line += '\\' + c
    else if (b >= 0x20 && b < 0x7f) line += c
    else line += '\\' + b.toString(8).padStart(3, '0')
    if (line.length >= 100) {
      out.push('"' + line + '"')
      line = ''
    }
  }
  if (line) out.push('"' + line + '"')
  return out.join('\n')
}

const assetBytes = ASSETS.map(([cid, file, mime]) => ({
  cid,
  file,
  mime,
  data: fs.readFileSync(path.join(TEMPLATE_ROOT, 'includes', file)),
}))

// ---- header
let h = `/* GENERATED by packages/email-native/tools/gen_c.mjs -- do not edit.
 * Source: the pug templates in packages/email-native/templates.
 * Regenerate with \`turbo @gradido/email-native#build\`.
 */
#ifndef SERVICE_CORE_EMAIL_TEMPLATES_H
#define SERVICE_CORE_EMAIL_TEMPLATES_H

#include "service_core/email/render.h"

typedef enum {
${ir.locales.map((l, i) => `    GE_LOCALE_${l.toUpperCase()} = ${i},`).join('\n')}
    GE_LOCALE_COUNT = ${ir.locales.length}
} ge_locale_t;

typedef enum {
${templates.map((t, i) => `    GE_TPL_${UP(t.name)} = ${i},`).join('\n')}
    GE_TPL_COUNT = ${templates.length}
} ge_template_t;

`

for (const t of templates) {
  const st = `ge_${camelToSnake(t.name)}_t`
  h += `/* ${t.name} */\ntypedef struct {\n`
  for (const s of t.slots) h += `    const char *${camelToSnake(s)};\n`
  for (const f of t.flags) h += `    bool ${camelToSnake(f)};\n`
  h += `} ${st};\n\n`
  h += `int ge_render_${camelToSnake(t.name)}(ge_locale_t locale, const ${st} *v, ge_mail_t *out);\n`
  h += `int ge_render_${camelToSnake(t.name)}_into(ge_locale_t locale, const ${st} *v, ge_arena_t *a, ge_mail_t *out);\n`
  h += `int ge_render_${camelToSnake(t.name)}_into_fast(ge_locale_t locale, const ${st} *v, ge_arena_t *a, ge_mail_t *out);\n\n`
}

h += `/* Introspection: enough to dispatch generically (a JSON API, say) without
 * writing code for each template. slot_names[0] is always "locale". */
typedef struct {
    const char         *name;
    uint32_t            prog_base;
    uint32_t            n_combos;
    uint32_t            n_slots;    /* including slot_names[0] == "locale" */
    const char *const  *slot_names;
    uint32_t            n_flags;
    const char *const  *flag_names;   /* NULL when the template has none */
    uint32_t            max_static_html;
    uint32_t            max_static_subject;
    uint32_t            max_static_text;
    uint32_t            max_slot_refs;
} ge_template_info_t;

extern const ge_template_info_t GE_TEMPLATES[${templates.length}];

/* Largest static share across ALL templates, locales and variants. The slot
 * values are added at runtime: at most 6 output bytes per input byte
 * (" -> &quot;), counted per slot USE rather than per slot -- some values
 * appear twice in a document (a link as a button and as plain text). */
#define GE_MAX_STATIC_HTML     ${Math.max(...templates.map((t) => t.max.html))}u
#define GE_MAX_STATIC_SUBJECT  ${Math.max(...templates.map((t) => t.max.subject))}u
#define GE_MAX_STATIC_TEXT     ${Math.max(...templates.map((t) => t.max.text))}u
#define GE_MAX_SLOT_REFS       ${Math.max(...templates.map((t) => t.max.refs))}u

/* Upper bound for one document if no input value is longer than max_field. */
#define GE_BUF_SIZE(max_field) \\
    (GE_MAX_STATIC_HTML + GE_MAX_STATIC_SUBJECT + 2u + 6u * 2u * GE_MAX_SLOT_REFS * (max_field))

/* The generic counterpart to the typed wrappers: picks the variant from the
 * same values, addressed by slot index rather than by struct field. This is
 * what a dispatcher works with -- an HTTP handler, or a Node-API addon that
 * only ever sees a name and an object. */
uint32_t ge_combo_for(ge_template_t t, const char *const *slots, const bool *flags);

int ge_render_values(ge_template_t t, ge_locale_t locale, const char *const *slots,
                     const bool *flags, ge_mail_t *out);
int ge_render_values_into(ge_template_t t, ge_locale_t locale, const char *const *slots,
                          const bool *flags, ge_arena_t *a, ge_mail_t *out);
int ge_render_values_into_fast(ge_template_t t, ge_locale_t locale, const char *const *slots,
                               const bool *flags, ge_arena_t *a, ge_mail_t *out);

/* Renders without the generated if-logic: 'combo' picks the variant directly.
 * slots[0] is filled in by the renderer. */
int ge_render_by_index(ge_template_t t, ge_locale_t locale, uint32_t combo,
                       const char *const *slots, ge_mail_t *out);
int ge_render_by_index_into(ge_template_t t, ge_locale_t locale, uint32_t combo,
                            const char *const *slots, ge_arena_t *a, ge_mail_t *out);
int ge_render_by_index_into_fast(ge_template_t t, ge_locale_t locale, uint32_t combo,
                                 const char *const *slots, ge_arena_t *a, ge_mail_t *out);

/* Lookup by name, for the server's HTTP dispatch */
int          ge_template_by_name(const char *name);   /* -1 if unknown */
int          ge_locale_by_code(const char *code);     /* -1 if unknown */
const char  *ge_template_name(ge_template_t t);
const char  *ge_locale_code(ge_locale_t l);

/* Inline attachments (cid:) -- these live in the binary too */
extern const ge_asset_t GE_ASSETS[${assetBytes.length}];
#define GE_ASSET_COUNT ${assetBytes.length}

#endif
`

// ---- implementation
let c = `/* GENERATED by packages/email-native/tools/gen_c.mjs -- do not edit.
 * Regenerate with \`turbo @gradido/email-native#build\`.
 */
#include "service_core/email/templates.h"
#include <stddef.h>
#include <string.h>

static const char GE_POOL[] =
${cStringLiteral(poolBytes)};

static const ge_op_t GE_OPS[] = {
${OPS.map(([o, l]) => `{${o},${l >>> 0}u}`).join(',').replace(/(.{110}[^,]*,)/g, '$1\n')}
};

static const ge_prog_t GE_PROGS[] = {
${PROGS.map(([s, n]) => `{${s},${n}}`).join(',').replace(/(.{110}[^,]*,)/g, '$1\n')}
};

static const char *const GE_LOCALE_CODES[] = {${ir.locales.map((l) => `"${l}"`).join(',')}};
static const char *const GE_TEMPLATE_NAMES[] = {${templates.map((t) => `"${t.name}"`).join(',')}};

const char *ge_locale_code(ge_locale_t l)     { return GE_LOCALE_CODES[l]; }
const char *ge_template_name(ge_template_t t) { return GE_TEMPLATE_NAMES[t]; }

int ge_template_by_name(const char *name) {
    for (int i = 0; i < GE_TPL_COUNT; i++)
        if (strcmp(GE_TEMPLATE_NAMES[i], name) == 0) return i;
    return -1;
}
int ge_locale_by_code(const char *code) {
    for (int i = 0; i < GE_LOCALE_COUNT; i++)
        if (strcmp(GE_LOCALE_CODES[i], code) == 0) return i;
    return -1;
}

/* Internal, as the trailing underscore says -- the per-template wrappers below
 * are the API.
 * prog = GE_PROGS[base + (locale * n_combos + combo) * 3 + kind], kind 0=html 1=subject 2=text */
int ge_emit_(uint32_t base, uint32_t n_combos, ge_locale_t locale, uint32_t combo,
             const char *const *sv, ge_mail_t *out) {
    if ((unsigned)locale >= GE_LOCALE_COUNT) return -1;
    const ge_prog_t *p = &GE_PROGS[base + ((uint32_t)locale * n_combos + combo) * 3];
    if (ge_run(GE_POOL, &GE_OPS[p[0].start], p[0].count, sv, &out->html) != 0) return -1;
    if (ge_run(GE_POOL, &GE_OPS[p[1].start], p[1].count, sv, &out->subject) != 0) {
        ge_str_free(&out->html);
        return -1;
    }
    if (ge_run(GE_POOL, &GE_OPS[p[2].start], p[2].count, sv, &out->text) != 0) {
        ge_str_free(&out->html);
        ge_str_free(&out->subject);
        return -1;
    }
    return 0;
}

/* Same into an arena: no malloc, no free. Both documents sit back to back in
 * the same buffer. */
int ge_emit_into_(uint32_t base, uint32_t n_combos, ge_locale_t locale, uint32_t combo,
                  const char *const *sv, ge_arena_t *a, ge_mail_t *out) {
    if ((unsigned)locale >= GE_LOCALE_COUNT) return -1;
    const ge_prog_t *p = &GE_PROGS[base + ((uint32_t)locale * n_combos + combo) * 3];
    /* On -1, out->html.len carries the total requirement for ge_arena_ensure(). */
    if (ge_run_into(GE_POOL, &GE_OPS[p[0].start], p[0].count, sv, a, &out->html) != 0) return -1;
    if (ge_run_into(GE_POOL, &GE_OPS[p[1].start], p[1].count, sv, a, &out->subject) != 0) {
        size_t need = a->used + out->subject.len;
        out->html.data = NULL;
        out->html.len  = need;
        return -1;
    }
    if (ge_run_into(GE_POOL, &GE_OPS[p[2].start], p[2].count, sv, a, &out->text) != 0) {
        size_t need = a->used + out->text.len;
        out->html.data = NULL;
        out->html.len  = need;
        return -1;
    }
    return 0;
}

int ge_emit_into_fast_(uint32_t base, uint32_t n_combos, ge_locale_t locale, uint32_t combo,
                       const char *const *sv, ge_arena_t *a, ge_mail_t *out) {
    const ge_prog_t *p = &GE_PROGS[base + ((uint32_t)locale * n_combos + combo) * 3];
    ge_run_into_fast(GE_POOL, &GE_OPS[p[0].start], p[0].count, sv, a, &out->html);
    ge_run_into_fast(GE_POOL, &GE_OPS[p[1].start], p[1].count, sv, a, &out->subject);
    ge_run_into_fast(GE_POOL, &GE_OPS[p[2].start], p[2].count, sv, a, &out->text);
    return 0;
}

`

for (const t of templates) {
  const fn = camelToSnake(t.name)
  const st = `ge_${fn}_t`
  const n = t.slotOrder.length
  // Fill the slots and pick the variant -- once, for all three entry points.
  c += `static void ge_prep_${fn}(ge_locale_t locale, const ${st} *v, const char **sv, uint32_t *combo) {\n`
  c += `    sv[0] = ge_locale_code(locale);\n`
  t.slots.forEach((s, i) => (c += `    sv[${i + 1}] = v->${camelToSnake(s)};\n`))
  c += `    uint32_t k = 0;\n`
  // combos were generated as the cartesian product in condition order
  for (const cond of t.conditions) {
    const nc = cond.cases.length
    const expr = cond.cases.slice(0, nc - 1).map((e, i) => `(${e}) ? ${i}u : `).join('') + `${nc - 1}u`
    c += `    k = k * ${nc}u + (${expr});   /* ${cond.id} */\n`
  }
  c += `    *combo = k;\n}\n\n`
  const call = `    const char *sv[${n}]; uint32_t combo;\n    ge_prep_${fn}(locale, v, sv, &combo);\n`
  const args = `${t.progBase}u, ${t.combos.length}u, locale, combo, sv`
  c += `int ge_render_${fn}(ge_locale_t locale, const ${st} *v, ge_mail_t *out) {\n${call}` +
    `    return ge_emit_(${args}, out);\n}\n\n`
  c += `int ge_render_${fn}_into(ge_locale_t locale, const ${st} *v, ge_arena_t *a, ge_mail_t *out) {\n${call}` +
    `    return ge_emit_into_(${args}, a, out);\n}\n\n`
  c += `int ge_render_${fn}_into_fast(ge_locale_t locale, const ${st} *v, ge_arena_t *a, ge_mail_t *out) {\n${call}` +
    `    return ge_emit_into_fast_(${args}, a, out);\n}\n\n`
}

const MAX_SV = Math.max(...templates.map((t) => t.slotOrder.length))

// v->logo_url -> slots[5], v->typo_correction -> flags[0]. Longest field name
// first, so one name cannot be rewritten inside another.
const comboCases = templates
  .filter((t) => t.conditions.length)
  .map((t) => {
    const subs = [
      ...t.slots.map((s, i) => [`v->${camelToSnake(s)}`, `slots[${i + 1}]`]),
      ...t.flags.map((f, i) => [`v->${camelToSnake(f)}`, `flags[${i}]`]),
    ].sort((a, b) => b[0].length - a[0].length)
    const rewrite = (e) => subs.reduce((acc, [from, to]) => acc.split(from).join(to), e)
    let body = `    case GE_TPL_${UP(t.name)}: {\n        uint32_t k = 0;\n`
    for (const cond of t.conditions) {
      const nc = cond.cases.length
      const expr =
        cond.cases.slice(0, nc - 1).map((e, i) => `(${rewrite(e)}) ? ${i}u : `).join('') + `${nc - 1}u`
      body += `        k = k * ${nc}u + (${expr});   /* ${cond.id} */\n`
    }
    return body + '        return k;\n    }\n'
  })
  .join('')

c += `
${templates.map((t) => `static const char *const GE_SLOTS_${UP(t.name)}[] = {${t.slotOrder.map((s) => `"${s}"`).join(',')}};`).join('\n')}
${templates.filter((t) => t.flags.length).map((t) => `static const char *const GE_FLAGS_${UP(t.name)}[] = {${t.flags.map((f) => `"${f}"`).join(',')}};`).join('\n')}

const ge_template_info_t GE_TEMPLATES[${templates.length}] = {
${templates.map((t) => `    { "${t.name}", ${t.progBase}u, ${t.combos.length}u, ${t.slotOrder.length}u, GE_SLOTS_${UP(t.name)}, ${t.flags.length}u, ${t.flags.length ? `GE_FLAGS_${UP(t.name)}` : 'NULL'}, ${t.max.html}u, ${t.max.subject}u, ${t.max.text}u, ${t.max.refs}u }`).join(',\n')}
};

int ge_render_by_index(ge_template_t t, ge_locale_t locale, uint32_t combo,
                       const char *const *slots, ge_mail_t *out) {
    if ((unsigned)t >= GE_TPL_COUNT) return -1;
    const ge_template_info_t *ti = &GE_TEMPLATES[t];
    if (combo >= ti->n_combos) return -1;
    const char *sv[${MAX_SV}];
    sv[0] = ge_locale_code(locale);
    for (uint32_t i = 1; i < ti->n_slots; i++) sv[i] = slots[i];
    return ge_emit_(ti->prog_base, ti->n_combos, locale, combo, sv, out);
}

int ge_render_by_index_into(ge_template_t t, ge_locale_t locale, uint32_t combo,
                            const char *const *slots, ge_arena_t *a, ge_mail_t *out) {
    if ((unsigned)t >= GE_TPL_COUNT) return -1;
    const ge_template_info_t *ti = &GE_TEMPLATES[t];
    if (combo >= ti->n_combos) return -1;
    const char *sv[${MAX_SV}];
    sv[0] = ge_locale_code(locale);
    for (uint32_t i = 1; i < ti->n_slots; i++) sv[i] = slots[i];
    return ge_emit_into_(ti->prog_base, ti->n_combos, locale, combo, sv, a, out);
}

int ge_render_by_index_into_fast(ge_template_t t, ge_locale_t locale, uint32_t combo,
                                 const char *const *slots, ge_arena_t *a, ge_mail_t *out) {
    const ge_template_info_t *ti = &GE_TEMPLATES[t];
    const char *sv[${MAX_SV}];
    sv[0] = ge_locale_code(locale);
    for (uint32_t i = 1; i < ti->n_slots; i++) sv[i] = slots[i];
    return ge_emit_into_fast_(ti->prog_base, ti->n_combos, locale, combo, sv, a, out);
}

/* The same branch decisions as the typed wrappers, with 'v->field' rewritten to
 * the slot index that field is filled from -- generated from one manifest, so
 * the two cannot say different things. */
uint32_t ge_combo_for(ge_template_t t, const char *const *slots, const bool *flags) {
    (void)slots;
    (void)flags;
    switch (t) {
${comboCases}    default: return 0;
    }
}

int ge_render_values(ge_template_t t, ge_locale_t locale, const char *const *slots,
                     const bool *flags, ge_mail_t *out) {
    if ((unsigned)t >= GE_TPL_COUNT) return -1;
    return ge_render_by_index(t, locale, ge_combo_for(t, slots, flags), slots, out);
}

int ge_render_values_into(ge_template_t t, ge_locale_t locale, const char *const *slots,
                          const bool *flags, ge_arena_t *a, ge_mail_t *out) {
    if ((unsigned)t >= GE_TPL_COUNT) return -1;
    return ge_render_by_index_into(t, locale, ge_combo_for(t, slots, flags), slots, a, out);
}

int ge_render_values_into_fast(ge_template_t t, ge_locale_t locale, const char *const *slots,
                               const bool *flags, ge_arena_t *a, ge_mail_t *out) {
    return ge_render_by_index_into_fast(t, locale, ge_combo_for(t, slots, flags), slots, a, out);
}
`

for (const a of assetBytes)
  c += `static const unsigned char GE_ASSET_${UP(a.cid)}[] = {${[...a.data].join(',')}};\n`

c += `
const ge_asset_t GE_ASSETS[${assetBytes.length}] = {
${assetBytes
  .map((a) => `    { "${a.cid}", "${a.file}", "${a.mime}", GE_ASSET_${UP(a.cid)}, sizeof GE_ASSET_${UP(a.cid)} }`)
  .join(',\n')}
};
`

fs.mkdirSync(path.join(OUT_DIR, 'service_core', 'email'), { recursive: true })
fs.writeFileSync(path.join(OUT_DIR, 'service_core', 'email', 'templates.h'), h)
fs.writeFileSync(path.join(OUT_DIR, 'templates.c'), c)

const kb = (n) => (n / 1024).toFixed(1) + ' KB'
const assetTotal = assetBytes.reduce((s, a) => s + a.data.length, 0)
console.error(`literals, raw       ${kb(rawTotal)}  (${lits.length} distinct)`)
console.error(`byte pool           ${kb(poolBytes.length)}  -> factor ${(rawTotal / poolBytes.length).toFixed(1)}x`)
console.error(`ops                 ${OPS.length} x 8 B = ${kb(OPS.length * 8)}`)
console.error(`programs            ${PROGS.length} (${templates.length} templates x ${ir.locales.length} locales x variants x {html,subject,text})`)
console.error(`max static share    html ${Math.max(...templates.map((t) => t.max.html))} B, subject ${Math.max(...templates.map((t) => t.max.subject))} B, text ${Math.max(...templates.map((t) => t.max.text))} B, up to ${Math.max(...templates.map((t) => t.max.refs))} slot uses`)
console.error(`assets (PNG)        ${kb(assetTotal)}`)
console.error(`expected data size  ~${kb(poolBytes.length + OPS.length * 8 + PROGS.length * 8 + assetTotal)}`)
