# @gradido/email-native

The e-mail templates as C, with no templates in the deploy.

The pug templates under `templates/` are the single source of truth. They are rendered at
build time — once per locale and once per branch variant — into a byte pool and a list of
ops, and that is what ships: in this package as a Node-API addon, and in `fast-servers` as
two C files linked straight into the binary. At runtime there is no template directory, no
parser and no i18n library — just `memcpy` and HTML escaping.

```text
templates/*.pug ─┐
locales/*.json  ─┤ tools/extract.mjs  (pug + i18n, build time only)
                 ▼
              ir.json                            intermediate format
                 │ tools/gen_c.mjs
                 ▼
   service_core/email_gen.{h,c} ──┬─► email_native.node   (this package)
                                  └─► fast-servers/service-core/  (copied by the build)
```

## The templates are imported, not referenced

`templates/` and `locales/` came from `gradido`'s `core/src/emails/templates` and
`core/src/locales` and are carried here verbatim. Legacy renders the same pug files at
runtime; from this repository's point of view they are source, and this is the package that
owns them. Both are excluded from biome in the root `biome.json`, so an import from legacy
stays a byte-for-byte copy rather than something a formatter has been over.

## What the build does

```sh
bun install
turbo @gradido/email-native#build     # or: bun run build, from this directory
```

`build` is `c-cpp-zig-build` followed by `scripts/sync-fast-servers.ts`. It runs the
snapshot check on the way (see below) and produces three things:

```text
build/email_native.node                    the addon: renderer + service-core's SMTP mailer
build/gen/service_core/email_gen.h         the generated renderer
build/gen/email_gen.c
fast-servers/service-core/{include,src}/   the same two files, plus email.h and email.c
```

The copy into `fast-servers` is why that build needs neither node nor pug: it compiles the
generated C rather than generating it. The four files it receives are checked in, so
`zig build` in `fast-servers` works in a fresh checkout — and they are **outputs**, not
sources. Change a template, run the build, commit what it wrote; never edit the copies in
`fast-servers`.

`build.zig` registers every file under `templates/` and `locales/` as a build input, so the
codegen re-runs exactly when a template changes. Zig hashes contents rather than mtimes, so
a bare `touch` triggers nothing, and an unchanged file is not rewritten in `fast-servers`
either — a rebuild that changed nothing leaves no `git status` behind.

Turbo caches `build`. If the files in `fast-servers` were removed by hand, the cache hit
will not put them back: `turbo @gradido/email-native#build --force` re-runs the copy.

## The snapshots

`tests/__snapshots__/` holds all 540 documents (17 templates × 10 locales × variants ×
{html, subject}) **as pug renders them**, rendered with test values containing everything
that can go wrong (`& < > " '` and non-ASCII) and checked into git — the same idea as
`__snapshots__` in `gradido`, one file per document:

```text
tests/__snapshots__/accountActivation/de.0.html
tests/__snapshots__/accountActivation/de.0.subject
```

The pug sources stay the source of truth. The snapshots are how a change to them becomes
**visible**: a pug upgrade, an edited template or a corrected translation shows up as a diff
over exactly the documents it reached, and over nothing else. Three things compare against
them, and none of them compares an implementation against itself:

```text
tests/snapshots.test.mjs   pug renders today what the snapshots say
tests/addon.test.js        the addon renders what the snapshots say   all 540
zig build check            the C binary renders what the snapshots say
```

The last one is `tools/verify.mjs` over what `tools/dump.c` wrote, and it needs no pug at
all — the first line is what keeps the reference honest. Together they are what backs "the C
sends the same mail as pug".

Both implementations are held to the **whole** matrix, every branch variant included.
`render()` has no variant argument — a variant is selected by which values are set, the way
the templates' `if`s do it — so `tools/variants.mjs` turns the branch table into those
values, and the numbering it produces is the one the generated C computes and the one the
snapshot filenames carry.

`check` is not a task of its own: it hangs off the default build step, so `npx czb` and
`turbo @gradido/email-native#build` run it. A build whose tables no longer match the
templates fails there rather than in a mail — and, more to the point, before
`scripts/sync-fast-servers.ts` copies anything into `fast-servers`.

**When a template, a locale catalogue or the pug version changes:**

```sh
bun run snapshots:update              # tools/snapshots.mjs, the only writer
git diff tests/__snapshots__          # read it -- this is the review
turbo @gradido/email-native#build     # regenerate the C, check it, copy it to fast-servers
```

An update that touches documents you did not expect is the finding, not the noise.

```sh
turbo @gradido/email-native#test      # the build ran the C check; these are the JS halves
bun run bench                         # sending and rendering, against nodemailer and pug
```

Bun's own snapshot mechanic (`toMatchSnapshot`, one `.snap` per test file) is not used here,
and that is a deliberate trade: three consumers have to read these files, and one of them is
a `zig build` step with neither bun nor pug in reach. Plain files are readable by all three,
diff per document rather than as one 5.6 MB blob, and let `tools/dump.c` write straight into
a comparable shape.

## How the branches are handled

The `if`s in the templates (`if logoUrl`, `if senderCommunityUuid && senderUuid`,
`timeDurationObject.minutes == 0`, the `if/else if/else` in `emailChangeSupport`) are **not**
translated into C. Every combination is rendered out as its own variant at build time and
the generated wrapper only picks the index.

That is the one part maintained by hand: `tools/manifest.mjs` says which variable drives
which branch and how C decides it. To keep the two from drifting apart, `extract.mjs` counts
the branches in the pug sources — following `include`/`extends` — and refuses to build when
the manifest does not match:

```text
accountActivation: 2 branch(es) in the pug sources, but 1 in the manifest.
      accountActivation/html.pug: if
      includes/requestNewLink.pug: if
      -> adjust tools/manifest.mjs (otherwise only one branch ships).
```

`each`/`case`/`while` trip the same wire — the extractor cannot handle them and does not
pretend otherwise.

The struct fields are **not** maintained by hand. The variable list is read off the compiled
pug function, so renaming a variable in a template fails the next build at the call site
with a compiler error rather than leaving a blank spot in the mail.

## Why it is 200 KB of data and not 5.4 MB

17 templates × 10 locales × variants × ~21 KB is 5.4 MB of literals, dominated by the 18 KB
of CSS inlined into every document. `tools/gen_c.mjs` does not store one literal block per
document: it assembles each literal greedily out of pool pieces that are already there
(LZ77-style, matches ≥ 48 bytes only). CSS, header and footer land in the pool exactly once.

| | |
|---|---|
| literals, raw | 5439.6 KB |
| byte pool | **101.7 KB** (factor 53.5×) |
| ops | 8452 × 8 B = 66.0 KB |
| inline PNGs (`cid:`) | 28.4 KB |

At runtime it is still a plain `memcpy` — nothing is decompressed.

## Buffer sizes

The static share is a build-time constant, because it is template bytes only:

```text
GE_MAX_STATIC_HTML     21962 B
GE_MAX_STATIC_SUBJECT    119 B
GE_MAX_SLOT_REFS          13      slot uses per document
```

User data comes on top, counted per **use** rather than per slot, and the most expensive
character is `"` → `&quot;`, six output bytes per input byte. **64 KB per thread** covers
every template as long as no field goes beyond 512 bytes; `bun run bench` measures it rather
than deriving it from a formula. The arena is checked, not assumed:

```c
ge_arena_t arena;
ge_arena_init(&arena, 64u << 10);        /* once at startup */

for (;;) {
    ge_mail_t m;
    ge_arena_reset(&arena);              /* per mail: used = 0, nothing else */
    if (ge_render_thank_you_card_paid_into(loc, &v, &arena, &m) != 0) {
        ge_arena_ensure(&arena, m.html.len);  /* m.html.len = the total requirement */
        ge_render_thank_you_card_paid_into(loc, &v, &arena, &m);
    }
    ...
}
```

## The mailer half, and why mbedTLS

The addon carries `fast-servers/service-core`'s `sc_mailer` as well, compiled straight out
of that checkout rather than vendored, so the two cannot drift apart. `-Dmailer=false`
leaves it out.

`curl` is built against **mbedTLS**, and that is what makes an addon viable at all: it lives
inside a process that already has a TLS library — OpenSSL in Node, BoringSSL in Bun — and a
second one exporting the same `SSL_*`/`EVP_*` symbols would leave link order to decide which
of them verifies a certificate. mbedTLS has its own API and its own symbol prefix. Verified
rather than assumed:

```sh
nm -D --defined-only build/email_native.node | grep -cE ' (SSL_|EVP_|OPENSSL_|CRYPTO_|X509_)'
0
```

`tests/tls.test.js` stands up an `smtps://` relay with a self-signed certificate and has the
addon deliver through it, then points the mailer at a certificate the relay was not signed
by and asserts that nothing is delivered. A build that only compiles would prove nothing.

### One exported symbol, and why it matters

`napi/exports.map` exports `napi_register_module_v1` and localises everything else. A
*global* symbol is preemptible, so with libuv, mbedTLS and arnm exporting their names from
the addon, its own `uv_cond_init()` would bind to whatever the **host executable** defines
under that name. Node defines the real one; Bun defines a stub that aborts the process,
which looked like "Bun cannot run threads in an addon" and is nothing of the sort. With the
version script the calls are settled at link time and the addon runs unchanged on both.

## Speed: the N-API boundary costs more than the render

`accountActivation/de`, 100k renders, one core, ns per mail:

| | Node 18 | Bun 1.3 |
|---|---|---|
| pug, compiled once | **4265** | 4452 |
| addon `render()` — two JS strings | 13776 | 38484 |
| addon `renderBytes()` — two Buffers | 7038 | |
| addon `sendTemplate()` — render + queue, no JS value | 26468 | n/a |

The C render itself is 0.6 µs; everything above it is the boundary. **pug is faster than the
addon whenever the document has to become a JS value** — which is the finding that matters
most here, and the reason this package exists for `fast-servers` first and for the
TypeScript path only where a mail never becomes a JS value.

`zigNative.optimize` is `fast`, not the `small` that `c-cpp-zig-build` defaults to: for a
memcpy-bound renderer `small` costs ~35 % (`render()` 21.3 µs vs. 13.8 µs).

Sending is the other way round — `sc_mailer` does ~9800 mails/s on Node against nodemailer's
~700 with `TCP_NODELAY` forced (86 without it, which is Nagle and not nodemailer).

## What it cannot do yet

- **`sc_mailer` sends `text/plain` only.** No `multipart/alternative` + `multipart/related`
  with the six `cid:` images. Both gaps are in `service-core`, not here.
- **No `text/plain` alternative to the HTML.** `email-templates` derives it from the HTML
  today; the same trick works — run `html-to-text` over the sentinel HTML at build time.
- **The subject line** is treated as text (entities decoded, slots not escaped). This is
  deliberately different from legacy, where the subject goes through pug and an `&` in a
  name arrives as `&amp;` in the subject line.
- **URL encoding.** Slots are HTML-escaped, including inside `href`. For UUIDs, IDs and
  ready-made URLs that is enough. A free-form string in a query parameter needs a third
  escape mode (`GE_SLOT_URL`) — the op already has the field for it.

## Layout

```text
templates/  locales/          imported from gradido, the single source of truth
tests/__snapshots__/          those templates as pug renders them, checked in
tools/                        extract.mjs and gen_c.mjs (build time), snapshots.mjs,
                              render_pug.mjs and variants.mjs (the snapshots),
                              verify.mjs (C vs. snapshots)
include/service_core/email.h  the renderer's public API, template-independent
src/email.c                   its runtime half: ops, escaping, the arena
napi/                         the Node-API bindings and the version script
tls/                          the mbedTLS trim (MBEDTLS_USER_CONFIG_FILE)
scripts/                      the copy into fast-servers
tests/                        node --test; the addon, its TLS, the snapshots, the
                              send benchmark as a test
```

`include/` and `src/` mirror the paths those two files have in `fast-servers/service-core`,
which is why there is one include spelling — `service_core/email.h` — and not two.
