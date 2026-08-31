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
   email/templates.{h,c} ──┬─► email_native.node   (this package)
                           └─► fast-servers/service-core/email/  (copied by the build)
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
build/email_native.node                          the addon: renderer + SMTP sending
build/gen/service_core/email/templates.h         the generated renderer
build/gen/templates.c
fast-servers/service-core/{include,src}/email/   the same two, plus render.h and render.c
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

`tests/__snapshots__/` holds all 810 documents (17 templates × 10 locales × variants ×
{html, subject, text}) **as pug renders them**, rendered with test values containing everything
that can go wrong (`& < > " '` and non-ASCII) and checked into git — the same idea as
`__snapshots__` in `gradido`, one file per document:

```text
tests/__snapshots__/accountActivation/de.0.html
tests/__snapshots__/accountActivation/de.0.subject
tests/__snapshots__/accountActivation/de.0.text
```

The pug sources stay the source of truth. The snapshots are how a change to them becomes
**visible**: a pug upgrade, an edited template or a corrected translation shows up as a diff
over exactly the documents it reached, and over nothing else. Three things compare against
them, and none of them compares an implementation against itself:

```text
tests/snapshots.test.mjs   pug renders today what the snapshots say
tests/addon.test.js        the addon renders what the snapshots say   all 810
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
diff per document rather than as one 5.8 MB blob, and let `tools/dump.c` write straight into
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
| literals, raw | 5645.2 KB |
| byte pool | **129.0 KB** (factor 43.8×) |
| ops | 14780 × 8 B = 115.5 KB |
| inline PNGs (`cid:`) | 28.4 KB |
| **data in the binary** | **~279 KB** for 810 programs |

At runtime it is still a plain `memcpy` — nothing is decompressed.

### And compressing it would cost more than it saves

Measured rather than assumed, because it is the obvious next idea: zstd takes the pool from
104 to 25 KB and the ops from 64 to 10 KB, so ~133 KB less data. A static binary that calls
nothing but `ZSTD_decompress`, `--gc-sections`, stripped, carries **227 KB of code** — the
decoder costs almost twice what it saves. zlib's `uncompress()` is 45 KB and would come out
ahead on paper, but only where zlib is already linked, which the addon does not.

The paging argument settles it either way. `.rodata` is demand-paged from the file: a
deployment that sends two of the ten locales never reads the pages of the other eight. A
decompressed blob is fully resident, as private dirty memory no kernel can drop, and `GE_OPS`
would lose its `const` on the way. Compression here trades disk for RAM, in the wrong
direction. If binary size ever becomes the constraint, drop locales at build time or compress
the deployment artifact — both have a better ratio than this.

## Buffer sizes

The static share is a build-time constant, because it is template bytes only:

```text
GE_MAX_STATIC_HTML     21962 B
GE_MAX_STATIC_SUBJECT    119 B
GE_MAX_STATIC_TEXT      2011 B
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

## Sending: one connection per mail, on the host's thread pool

The addon does **not** use `sc_mailer`, service-core's pooled mailer. It compiles the two
layers underneath it — straight out of the fast-servers checkout rather than vendored, so the
two paths cannot send different mail:

```text
service-core/src/email/message.c     the RFC 5322 bytes: headers, RFC 2047 subject, CRLF
service-core/src/email/transport.c   one curl session: connect, authenticate, hand over
-----------------------------------------------------------------------------------------
service-core/src/email/mailer.c      the queue, the retry and the worker pool -- not here
```

Every `send()` is a `napi_create_async_work`: `execute()` opens a session on a libuv thread
pool thread, hands over one message and closes it; `complete()` settles a `Promise` on the JS
thread with the Message-ID, or rejects it with the relay's own words. That is the same shape
nodemailer has by default, and it is the reason **this addon links no libuv and no arnm at
all** — the two files above have neither threads nor an arena.

Two costs come with it, and both are the trade rather than an oversight:

- **A connection per mail is a TCP handshake, a TLS handshake and the SMTP greeting dialogue.**
  On loopback that is 225 µs against 85 µs on a kept session; against a remote relay it is
  round trips, so hundreds of milliseconds.
- **The pool has four threads** (`UV_THREADPOOL_SIZE`), shared with `fs`, `dns` and `crypto`,
  and a send holds its thread for the whole session. So the wrapper takes at most
  `UV_THREADPOOL_SIZE - 1` of them and queues the rest: filling every thread with mail would
  stall every file read in the process for as long as the relay takes. `maxConcurrent`
  overrides it, `stats.limit` says what it resolved to, and `stats.waiting` how many sends are
  holding back.

  The gate is in `index.cjs` rather than in C, and that is a memory decision: a send waiting
  there holds a recipient and a few values, one queued on the C side would already hold its
  formatted message — ~64 KB with the images. A thousand queued mails is the difference between
  a rounding error and 64 MB.

For registration and notification mail — one mail per request, and the caller wants to know
whether it went — that is the right trade. Bulk mail is what `fast-servers` and its pool are
for. `-Dmailer=false` leaves the sending half out entirely.

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
*global* symbol is preemptible, so a name the addon exports and the host also defines is
resolved by link order rather than by which library the call came from. That bit hard once:
while the addon still linked its own libuv, its `uv_cond_init()` bound to Bun's shim, which
aborts the process — it looked like "Bun cannot run threads in an addon" and was nothing of
the sort. libuv is gone now, but mbedTLS and curl are still in here, so the version script
stays: one exported symbol, and ~148 KB of `.dynsym`/`.dynstr`/`.hash` saved with it.

## Speed: the N-API boundary costs more than the render

`accountActivation/de`, 100k renders, one core, ns per mail:

| | Node 18 | Bun 1.3 |
|---|---|---|
| pug, compiled once | **4338** | **3944** |
| addon `render()` — three JS strings | 15746 | 31553 |
| addon `renderBytes()` — three Buffers | 7985 | 4569 |
| `send()`, the synchronous half — render + MIME, no JS value | 94977 | 93229 |

The C render itself is 0.6 µs; everything above it is the boundary. **pug is faster than the
addon whenever the document has to become a JS value** — which is the finding that matters
most here, and the reason this package exists for `fast-servers` first and for the
TypeScript path only where a mail never becomes a JS value.

The last row is not the boundary, it is the encodings: quoted-printable over 21 KB of HTML and
base64 over 28 KB of inline images, which is what turns a 21 KB document into a 64 KB message.
It caps one JS thread at ~11k mails/s — far above what one connection per mail delivers, so it
is a number to know rather than one to optimise.

`zigNative.optimize` is `fast`, not the `small` that `c-cpp-zig-build` defaults to: for a
memcpy-bound renderer `small` costs ~35 % (`render()` 21.3 µs vs. 13.8 µs).

Sending, `bun run bench`: 1000 mails, 20897 B body, 4 in flight, relay in its own process,
Node 18 on one machine — µs per mail:

| | plain | smtps |
|---|---|---|
| addon, one connection per mail | **233** | **1529** |
| nodemailer, pooled, as configured | 11299 | 11376 |
| nodemailer, pooled, `TCP_NODELAY` forced | 1380 | 1408 |

**Nagle** is nodemailer's second row: curl sets `TCP_NODELAY`, Node does not, so each message
eats a delayed-ACK stall. Forcing it is worth 8x, and Bun sets it already — so that row is the
one to compare against.

**What the connection per mail costs.** Plain SMTP against a held connection is 85 µs and
against a new one 225 µs (`email/transport.h`, measured in the h2o prototype) — and 233 µs is
exactly where this lands, so on plain SMTP the price of not pooling is that ~2.5x. Over TLS it
is 233 → 1529 µs, and the extra 1300 µs is a handshake per mail: asymmetric crypto on *both*
sides plus its round trips. That is the number the design decision cost, and against a remote
relay it would be larger still, because then the round trips are milliseconds rather than
microseconds.

**Threads are not where it comes from.** The sends scale with what is in flight and then stop:

```text
1 in flight   2046 mails/s        UV_THREADPOOL_SIZE=8,  8 in flight   3507 mails/s
2             2799                UV_THREADPOOL_SIZE=16, 16           3589
4             3403
8             3363
```

The ceiling is the single-process JS relay, not the pool — raising `UV_THREADPOOL_SIZE` moves
nothing. A pool of dedicated threads would hit the same wall here, which is why the addon
giving up its own threads costs nothing measurable and the handshake costs everything.

For comparison, service-core's pooled `sc_mailer` measured **102 µs plain and 194 µs over
smtps** in the prototype this package came from. Those are from another machine and another
run, and nothing here reproduces them — the addon no longer contains that path. What would
settle it is a throughput benchmark beside `fast-servers/benchmarks/`, which is where the
pooled mailer lives now.

## The message on the wire

What leaves the process, and both paths put out the same bytes —
`service-core/src/email/message.c` writes them for the addon and for `fast-servers`:

```text
multipart/alternative
  text/plain; charset=utf-8                  quoted-printable
  multipart/related; type="text/html"        the templates' six cid: images ride along
    text/html; charset=utf-8                 quoted-printable
    image/png x6                             base64, Content-ID: <...>, inline
```

The text part comes off the same document: `tools/extract.mjs` runs `html-to-text` over the
**sentinel** HTML at build time, so the slots survive into a third program and C fills them at
runtime — see `TEXT_OPTIONS` in `tools/manifest.mjs` for the four decisions that took. Text
first and HTML second, because RFC 2046 5.1.4 puts the alternatives in increasing order of
faithfulness. `sendMail()` takes `text`, `html` or both and builds the same shapes.

| | |
|---|---|
| `Date:` | RFC 5322 3.3, UTC, names spelled out — a `de_DE` process would write "Mo, 25 Aug" |
| `Message-ID:` | RFC 5322 3.6.4, and it survives the retry, so one mail is one id |
| `Subject:` | RFC 2047 base64 encoded-words when not ASCII, folded, split on character boundaries |
| `From:` | display name quoted when it carries a special, RFC 2047 encoded when not ASCII |
| addresses | a control character is refused — in the recipient and in the sender |
| bodies | quoted-printable: 7-bit, and no line past 76 characters |
| text part | derived from the HTML at build time, slots filled with `GE_SLOT_RAW` |
| images | base64, `Content-ID` matching the `cid:` the HTML names, `Content-Disposition: inline` |
| line endings | CRLF throughout, dot stuffing left to curl (RFC 5321 4.5.2) |

Verified rather than asserted. `tests/addon.test.js` sends one through a relay and checks the
wire: the structure, the order of the alternatives, one `Content-ID` per asset, no byte above
126, no line past 998, and the quoted-printable decoded back **byte for byte equal to what
`render()` produced** — an encoding that loses a byte would pass every other check.
`service-core/tests/test_mail.cpp` covers the same from the C side, part by part.

The text part is held to pug the same way the HTML is: `zig build check` renders all **810**
documents from the C binary and compares them against the snapshots, and those are what
`html-to-text` produced from pug's output. The two sides do it in opposite orders — the build
converts the sentinel HTML and lets C fill the slots, the check fills the slots and converts
afterwards — so they agree only because `wordwrap` is off. A value with a line break in it
would diverge, and the fixture values deliberately have none.

Two things this cost, and both are the trade rather than a surprise:

- **A mail is now ~64 KB rather than ~21 KB.** The six images are 28 KB and base64 adds a third.
  That is what an HTML mail with inline images weighs; the alternative is hosting them, which
  the templates do not do.
- **`SC_MAIL_MESSAGE_DEFAULT` went from 32 to 96 KiB**, and the pooled mailer reserves
  `queue_max` of those — ~24 MB rather than ~8 MB for the default queue of 256. A deployment
  that sends fewer at once should say so with `queue_max`.

### What is still missing

- **SMTPUTF8** (RFC 6531) is not negotiated, so an internationalised address is passed through
  rather than rejected. Every address this project has is ASCII; when one is not, that is a
  feature to add and not a byte to refuse.
- **The subject line** is treated as text (entities decoded, slots not escaped). This is
  deliberately different from legacy, where the subject goes through pug and an `&` in a name
  arrives as `&amp;` in the subject line.
- **URL encoding.** Slots are HTML-escaped, including inside `href`. For UUIDs, IDs and
  ready-made URLs that is enough. A free-form string in a query parameter needs a third escape
  mode (`GE_SLOT_URL`) — the op already has the field for it.

## Layout

```text
templates/  locales/          imported from gradido, the single source of truth
tests/__snapshots__/          those templates as pug renders them, checked in
tools/                        extract.mjs and gen_c.mjs (build time), snapshots.mjs,
                              render_pug.mjs and variants.mjs (the snapshots),
                              verify.mjs (C vs. snapshots)
include/service_core/email/    render.h, the renderer's public API
src/render.c                   its runtime half: ops, escaping, the arena
napi/                         the Node-API bindings and the version script
tls/                          the mbedTLS trim (MBEDTLS_USER_CONFIG_FILE)
scripts/                      the copy into fast-servers
tests/                        node --test; the addon, its TLS, the snapshots, the
                              send benchmark as a test
```

`include/` and `src/` mirror the paths those two files have in `fast-servers/service-core`,
which is why there is one include spelling — `service_core/email.h` — and not two.
