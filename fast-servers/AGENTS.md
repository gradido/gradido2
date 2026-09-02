# AGENTS.md — fast-servers/

Rules for working on the C implementation.

**Read `../AGENTS.md` first** for the project rules, and `Architecture.md` next to this file
for the design. This file states only what changes when the language is C.

---

## 0. What this path is, and what it may not become

`fast-servers/` is C: h2o, the request path, the session cache, the repositories. It is an
independent implementation of the same business behavior as `packages/`, and it is allowed
to lag behind.

Two rules from `../AGENTS.md` govern everything here:

- **No feature originates here.** If behavior exists only in C, it has silently removed
  itself from the TypeScript path — which is the path that keeps the project alive if its
  author is not around. Write it there first, always.
- **This path must be droppable, not merely removable.** Running the product without it must
  need no code change anywhere: no shared state, no route only served here, no role only
  filled here.

And one that governs how this path is deployed:

- **A deployment runs this path or the TypeScript path, never both.** A route not implemented
  here answers `ROUTE_NOT_IMPLEMENTED`; it is never proxied to TypeScript, because the session
  cache is in-process and per implementation. Lagging behind is a smaller route set on a
  fast-path deployment, not a mixed deployment. `../Architecture.md`, *One implementation per
  deployment*, has the reasoning.

When TypeScript changes: identify whether business behavior changed, locate the
corresponding domain path here, assess whether it is affected, update only when required.
Do **not** force artificial parity. Preserve the business semantics, write idiomatic C.

`README.md` beside this file holds the layout, the build commands and the options.

Comments in here and in the sources refer to **the h2o prototype**: a separate project, not in
this repository and not published, where the h2o request path, the JWT verifier, the session
cache and the fallback server were written and measured before any of it moved here. What it
established is written down in `Architecture.md`; what it produced was carried over rather than
depended on. Nothing in this repository reaches for it, and no path to it belongs in a file that
is published — this one included.

---

## 1. House dialect

So that review stays uniform and generated code stays checkable:

```text
- no malloc in the request path, arena only
- fixed buffer sizes with an explicit bounds check;
  overflow answers 500, never truncates
- one ownership convention, describable in one line
- function pointers at registration only, not in the data flow
```

Nothing on the session read path allocates. See `Architecture.md`, *Session cache*, for why
that is still an open constraint rather than a solved one.

Two exceptions exist. Both are written down where they are, so that neither spreads by being
mistaken for the rule:

```text
http_fallback.c   one calloc per connection, 80 KiB of buffers. Affordable
                  only because that backend is the one not carrying load —
                  do not carry the shape over to the h2o path.
log.c             a line that would not fit is truncated. The one place
                  truncating beats failing: the alternative is losing the
                  event entirely, and the structure around it is never
                  truncated, only the human sentence.
```

Anything else that wants to allocate per request is a design change, not a patch.

---

## 2. C++ and Rust are for leaf modules only

Justified by a library without a C equivalent — `gradido-blockchain-core`, signing, hashing.
**Not** by the convenience of a container: `std::unordered_map` allocates per insert and is
the wrong structure for a server that allocates nothing per request. Use open addressing
with a preallocated table, or a sorted array for ordered access.

A C++ module:

```text
- exports an extern "C" header and nothing else
- lets no C++ type cross the module boundary
- is compiled with -fno-exceptions -fno-rtti
```

The reason is concrete: an exception propagating into h2o's C event loop is undefined
behavior.

**Rust follows the same rule, in one module: `dht-node`.** It is there because libp2p has no
C equivalent, not because Rust is nicer, and the boundary is the same shape:

```text
- exports an extern "C" header and nothing else
- lets no Rust type cross the module boundary
- #![forbid(unsafe_code)] in the interior; the unsafe lives in one file
- panics are caught at the boundary and become an error code
```

The last line is the Rust version of the exception rule: a panic unwinding into h2o's C
event loop is undefined behavior for exactly the same reason a C++ exception is.

Do not add a second Rust module. If one looks necessary, change `../Architecture.md` first —
a third toolchain on the fast path is a design decision, not a dependency.

**Tests are not modules.** The unit tests are C++ because googletest is, calling `extern "C"`
headers — the same arrangement arnm and gradido-blockchain-core use. This section is about
what runs inside a server; nothing under `<component>/tests/` is linked into one.

---

## 3. zig builds, it does not implement

Build system and cross compiler. No application code — its API still moves between versions.

`build.zig.zon` declares `minimum_zig_version = "0.15.1"`. That is a floor, not the pin:
`../AGENTS.md`, *Toolchain*, holds where the pinned toolchain comes from and why the number is
worth reading rather than guessing.

`CMakeLists.txt` mirrors `build.zig` and exists for the one target zig cannot serve, the MSVC
ABI. When the two disagree, `build.zig` is right.

---

## 3a. Dependencies

Same policy as `../AGENTS.md` section 13, and it already holds here: `build.zig.zon` pins
every dependency to a fixed commit with a hash. Keep it that way — a floating dependency in
a C build is a floating dependency in the process that signs transactions.

What is in, and what each is for:

```text
blockchain_core    the money arithmetic, the wire formats, the crypto.
                   Same commit packages/shared-native pins, so both builds
                   in this repository see one layout of the same structs.
libsodium          HS256, for the JWT. Same pin and the same options the
                   core requests, or the build gets two instances of it.
arnm               the arena, the containers, the conversions and the JSON
                   the core is written against — arnm_result is what a grd*
                   call answers with. Same pin and options as the core.
h2o                the fast HTTP backend, and the picohttpparser the other
                   backend compiles. Fetched by every build for that reason.
curl               libcurl, for service-core's mail client and for the
                   outbound HTTP this project will grow. Pinned at the last
                   commit of allyourcodebase/curl that still declares zig
                   0.15.1; everything after it wants 0.16. Its TLS is mbedtls,
                   which curl pins itself -- see the note below.
libressl    lazy   the TLS h2o is built against and the TLS libpq speaks to
                   the database. h2o cannot be built without an OpenSSL *API*
                   -- <openssl/ssl.h> is in h2o.h with no #ifdef around it --
                   and LibreSSL is one; socket.c branches on
                   LIBRESSL_VERSION_NUMBER in four places. Not
                   allyourcodebase/openssl: that package is x86_64-only by
                   construction, which costs every arm64 build. Pinned to the
                   commit libpq names, hash included, so the two are one
                   package in the graph. Not fetched on Windows, where h2o is
                   off, curl gets Schannel and libpq is not built.
zlib        lazy   h2o's gzip handler. Same pin as curl's, same reason.
libuv              the platform layer — see below. Every build links it.
googletest  lazy   the unit tests.
compile_commands   feeds compile_commands.json.
libpq       lazy   the PostgreSQL driver, built by allyourcodebase/libpq out
                   of a pinned postgres checkout — so that pin carries two,
                   and 5.18.4 is PostgreSQL 18.4. Requested with `ssl =
                   .LibreSSL`, its own default, which is also the entry above:
                   both resolve to one package, so the process holds one
                   OpenSSL-API library and not two that export the same
                   symbols. Lazy: the postgres checkout is 155 MB and
                   `-Dpostgres=false` never fetches it. Not built on Windows —
                   the package has no port for it, and `-Dpostgres=true` there
                   stops the build with that sentence rather than with 92
                   errors inside a dependency.
sqlite3            SQLite, as sqlite.org's amalgamation. One C file, compiled
                   by build.zig the way picohttpparser is, rather than through
                   a package whose build.zig would add a toolchain floor for
                   one translation unit.
```

`lazy` means a build that does not select that path never downloads it.

**Two TLS libraries, and it is a waiting position rather than a design.** h2o and libpq speak
LibreSSL; libcurl speaks mbedtls, because curl's package cannot be handed a LibreSSL — where it
chooses, its `build.zig` reads `// TODO BoringSSL, AWS-LC, LibreSSL, and quictls`. Two libraries
with the *same* API would be the yyjson problem again, with link order deciding which one
verifies a certificate; mbedtls has an API and a symbol namespace of its own, so the two
coexist. **When that TODO lands, move libcurl onto LibreSSL and delete this paragraph** — one
TLS library is the intended state, and the second one is here because a package is not finished
yet, not because the mail client wants a different stack from the server.

### libuv is the platform layer

Threads and synchronisation now, filesystem, DNS and child processes as they are needed. One
dependency where there would otherwise be four `#ifdef _WIN32` shims written at four different
times. It earns its place by what it bundles, not by any one part: for threads alone it would be
49 000 lines against a 150-line shim. The decision is that everything it offers and this project
needs goes through it, so the platform seam exists once and is maintained once.

Two rules come with it, and `Architecture.md`, *Platform layer*, holds the reasoning:

```text
loop-free  uv_thread_*, uv_mutex_*, uv_rwlock_*, uv_cond_*, uv_sem_*, uv_once, uv_key_*
           usable as they are, next to h2o's own evloop
loop-bound everything asynchronous — uv_fs_*, uv_getaddrinfo, uv_spawn, uv_queue_work
           needs a uv_loop_t, and the process has h2o's. Do not start a second one on
           the request thread; put it on a thread of its own or change h2o's backend.
```

**Before adding a thread anywhere, read `Architecture.md`, *Threading*.** It holds one model for
the whole process, and the rule it comes down to is that work leaves the request path because it
serialises or syncs, never because it waits — waiting that has a file descriptor belongs on the
loop. A module that decides its own threading is how a process ends up with four answers to one
question.

The loop-free half is used directly and not wrapped: `uv_rwlock_t` in the session cache,
`uv_mutex_t` in the log, `uv_thread_t` for the thread each role runs on, `uv_once` for the
cache's hash seed. There is no `service_core/thread.h` to go through, and there must not be one
again — a wrapper is the shim under another name.

What libuv does not offer stays ours: `service_core/atomic.h` is four functions over the
compiler's builtins, because libuv has no atomics and `<stdatomic.h>` is behind an experimental
switch on MSVC, which the CMake build has to compile.

**Fetch, do not vendor.** Two files nobody would ever diff against the original again are worse
than a download — which is why picohttpparser is taken out of the pinned h2o checkout rather
than copied in. If something looks too small to be worth pinning, that is an argument for not
depending on it at all, not for copying it in.

**Watch what the core starts carrying.** This build pinned yyjson itself while blockchain-core
was at 0.16.0 and had no parser; 0.17.0 began linking libarnm, which carries one, and the pin
became two definitions of every `yyjson_*` symbol with link order deciding between them. When
the core takes on a dependency this build also names, one of the two has to go — and it is this
one, because a consumer that pins around its own library is pinning twice.

**Reach for arnm's surface, not for what is under it.** `arnm/json_reader.h` and
`arnm/json_writer.h` let no yyjson type, constant or include path through, so the parser
underneath can be replaced without this repository hearing about it. The same holds for the
allocator and the conversions. Going around
them to the vendored source is how a build ends up pinned to an implementation detail of a
dependency of a dependency.

Prefer no dependency at all. A library that saves fifty lines of C does not earn its place.

---

## 3b. h2o, and the fallback behind the same header

h2o is the server. `service_core/http.h` has a second implementation behind it, and it is not
an alternative to h2o — it is what the Windows build gets instead of nothing:

```text
http_h2o.c        h2o. This is the fast path, and every performance figure in
                  ../Architecture.md is about it: HTTP/1.1 and HTTP/2, 11.6 µs
                  for a cached request. The default wherever it builds.
http_fallback.c   libuv + picohttpparser and about a hundred lines of framing.
                  One thread, one event loop, HTTP/1.1, no TLS. It exists for
                  one reason: h2o is a posix event loop and does not compile
                  against the MSVC runtime.
```

It is not a second HTTP library and must not become one. Owning the accept loop is what keeps
the handler signature single; Mongoose is GPLv2 or commercial and is not the way out.

**The fallback is not a deployment option.** One thread means one core, whatever the machine
has, and nothing about it was built to carry load. It clamps `SERVER_THREADS` to one and logs
that it did, rather than refusing to start on a configuration h2o accepts -- the seam's promise
is that the same configuration starts both. It is there so that everything around the
request path — the roles, the configuration, the domain code — can be worked on and debugged
where h2o cannot build. A high-performance server on Windows is not on the table anyway; the
fast path targets the Linux machine this project runs on. `-Dh2o=false` selects the fallback and
Windows forces it, and there is no third reason to choose it.

Three rules follow, and the first is the one that keeps the seam worth having:

- **A role never asks which one is underneath.** `sc_http_backend_name()` exists for the startup
  line and for `--version`. Code that branches on it has put behavior on one backend and not the
  other, which is the same failure as putting a feature on the fast path and not in TypeScript.
- **A change to one is a change to both, or it is a divergence.** Every difference a *client*
  can observe is listed in `tests/integration/README.md` and asserted in the suite, so none of
  them moves unnoticed. The one to know before either goes behind a proxy: `Content-Length`
  together with `Transfer-Encoding` — h2o serves the request, the fallback refuses it.
- **A new route goes behind the header, never into a backend.** `tests/integration/probe/` is
  the worked example of a second consumer of that header.

---

## 3c. E-mail: five files, three layers, and two of them are copies

Everything about mail lives in `service-core/{include/service_core,src}/email/`, and the split
is what lets the Node addon in `packages/email-native` send the identical message without any
of the machinery a server needs:

```text
message.{h,c}    the bytes of one mail: the headers, and a MIME document --
                 multipart/alternative over text and html, multipart/related for
                 the inline images, quoted-printable and base64 for the parts.
                 No clock, no allocation, no I/O
transport.{h,c}  one SMTP session over curl: connect, authenticate, hand over one
                 message. No threads, no logging, no queue. <curl/curl.h> reaches
                 no further than this file
mailer.{h,c}     the bounded queue, the retry and the growing worker pool. The only
                 one with libuv in it -- and the only one the addon does not compile
render.{h,c}     the template renderer's runtime: ops, escaping, the arena
templates.{h,c}  generated from the pug templates -- 560 KB, and not a review task
```

**`render` and `templates` are copies**: their source is `packages/email-native` and they are
outputs of its build.

**Do not edit those two in this tree.** Change the template or `src/render.c` in
`packages/email-native`, run `turbo @gradido/email-native#build`, and commit what it wrote. An
edit made here is lost at the next build of that package and, worse, silently makes the two
implementations send different mails in the meantime. `message`, `transport` and `mailer` are
this repository's own and are edited here -- but a change to the first two reaches the addon,
which compiles them, so build that package afterwards.

They are checked in rather than generated by `build.zig` because the codegen is
`node tools/gen_c.mjs` and pug is a JS library: this build stays free of node, bun and
`node_modules`, which is what "droppable, not merely removable" needs in the other direction
too. What backs the copies is that package's build itself: before it writes anything here,
it renders all 810 documents from the C and compares them against its checked-in snapshots,
which are the pug output and are held to it by its own test suite.

`ge_*` is the renderer's own prefix and stays; the files carry it through unchanged from the
package that generates them, so the diff of a template change is the template's, not a
renaming's. Nothing links them yet -- no role sends mail -- so the linker drops the objects,
and they cost the binary nothing until the first caller.

**Where a change belongs.** A mail that arrives wrong on the wire -- a header, an encoding, a
part, a line ending -- is `message.c`, and fixing it there fixes the addon too. What that file
answers to is the RFCs, not this repository: 5322 for the headers, 2045/2046 for the parts,
2047 for the subject, 2387 for the related bundle, 5321 for what SMTP does to it. Something about the
relay -- TLS, auth, timeouts -- is `transport.c`, same reach. Only what is about *load* --
how many sessions, how long a mail waits, what happens after a failure -- belongs in
`mailer.c`, and that one is this path's alone.

---

## 4. Safety net

Not optional, and less so where the code was AI-generated:

```text
ASan + UBSan + TSan in CI, not only locally.
    TSan matters most — the session cache's reference counting and its
    two lock layers are the one defect class expert review does not catch.
Fuzzing for every parser touching attacker-supplied bytes: JWT, JSON.
Contract vectors as a merge gate, green on both implementations.
```

A data race here does not fail a test. It fails in production, under load, weeks later.

How to run them:

```text
zig build -Dtests -Dsanitize=thread test              TSan
zig build -Dtests -Dsanitize=undefined_behavior test  UBSan
cmake -B build -DFS_ENABLE_SANITIZERS=ON              ASan, and only here — zig
                                                      ships no asan runtime, which
                                                      is half of why CMakeLists.txt
                                                      exists at all
```

What the list above still asks for and does not have, named rather than left to be discovered:

```text
fuzzing            nothing is fuzzed. service-core/src/jwt.c is the first
                   candidate — base64 and JSON, both read before anything
                   has been vouched for — and picohttpparser is the second,
                   now that a backend of ours runs it.
contract vectors   contracts/test-vectors is empty, so the merge gate has
                   nothing to run yet.
```

---

## 5. Before you touch the session cache

Read `Architecture.md`, *Session cache*, in full. The invariants there are not style
preferences and each of them was a bug first:

```text
the slot is a reference; freed exactly when the count reaches zero
the reference count is incremented INSIDE the store lock, never after releasing it
the store lock is released before any session lock is taken
data-set locks are acquired in a fixed order
no lock upgrade — release shared, take exclusive, check again
no session lock is ever held across a database call
the store is asked BEFORE the signature is verified, because verifying costs more than
    looking, and nothing an unverified token says is trusted as data
the claim's TTL is checked before the store is touched — a filter, not a guarantee; the
    ENTRY's own session_created_at is what ends a session
a missing slot index in the token is a miss, never slot 0; the index is range-checked
    against the slots that exist, and the entry's user_uuid is compared to the claim
the token itself is compared against the set of tokens the session was issued, under the
    SESSION's lock and after the store lock is released — a hit is that comparison
the signature is verified only on the miss path, which is the path that creates a session
creation order lives in a queue of slot indices, never in the order of the slots — a slot
    is reused as soon as its session ends, and is then out of turn
a LIVE session is never moved: its slot number is out in the world, inside tokens this
    process signed. The vector grows by appending, and reuse goes through the free list
the configured maximum is a crash guard, not a size. Below it nothing is retired early;
    at it the oldest live session is, and session.context.evicted says so
```

The store is not keyed and not hashed: the JWT carries the slot index and a lookup is an array
read. Do not reintroduce a hash here. `Architecture.md`, *What was measured*, has the two
reproductions of what that costs — a colliding key set does not make a bounded-probe cache slow,
it makes it stop holding anything, with correct answers, the hit rate on the floor and nothing in
the log. That failure is unreachable when the server hands out the slot; the seeded splitmix mix
remains the rule for any cache that still derives a slot from a key.

The store half of it is `service-core/src/cache.c`, and `service-core/tests/test_cache.cpp`
covers it. Run that under TSan and not only plain: its concurrent test proves little on its own,
because a reference count incremented outside the store lock does not fail an assertion.

**That file is still the hash-routed table this design replaced.** It keys entries by a digest of
the token, probes, and knows nothing about slots or token sets — the document is ahead of it, and
where they disagree the document is right. Whoever brings it up to date writes the direct index,
not a fix to the hashing.

One thing is still open: how a session's working set grows while only a shared lock is held. It
is in `Architecture.md`, *Open*, and nothing in the cache decides it.

---

## 6. Known idioms

Record here what keeps being reinvented or mis-remembered, so the next agent does not
rediscover it:

```text
h2o    register the generator before the query goes out, not after —
       otherwise a client disconnect writes into a freed request
h2o    the request pool lives exactly as long as the request; anything
       the answer outlives it must not come from there
h2o    h2o_send_inline does NOT set res.content_length. Its own comment
       says why — it also serves 304s — and without it the HTTP/1 layer
       answers chunked, which the other backend never does. Set it before
       sending. curl hides this; the integration suite is what caught it.
h2o    max_request_entity_size defaults to a gigabyte. Both backends have
       to refuse at SC_HTTP_MAX_BODY, so the server sets it at startup.
h2o    the head limit is H2O_MAX_REQLEN, a compile-time constant of about
       400 KiB. There is no knob. The 8 KiB limit is the fallback's own.
h2o    it sends `Server: h2o/<version>` on every response until globalconf
       .server_name is emptied — and from a git checkout that version reads
       "2.3.0-DEV", which announces an unreleased build. Not a vulnerability
       and not a defence: fingerprinting works on header order anyway. It
       denies the scanner that shortlists by banner, and costs one line.
       Neither backend sends one; the integration suite asserts that.
h2o    h2o_start_response asserts the generator is NULL, and h2o_send_inline
       calls it itself. A request that was deferred therefore cannot be
       answered with h2o_send_inline -- it already has a generator. Send
       with h2o_send and H2O_SEND_STATE_FINAL instead; do_sendvec clears
       the generator before sending, which is also what keeps `stop` from
       firing on a request that WAS answered.
h2o    it stops reading a connection while a response is pending, so a
       client that closes after its request is usually not noticed until
       the write fails. The fallback keeps reading and sees the EOF. Do
       not build anything on being told; build on the request staying
       alive until it is answered, which is what the generator gives you.
http   a deferred request's ticket packs loop, slot and generation, and the
       slot's generation and phase share one word so that validating a
       ticket and claiming it are one CAS. Two steps is a race: a slot
       released and re-armed in between passes both checks. http_defer.h
http   the Windows fallback is libuv + picohttpparser + ~100 lines, not a
       second HTTP library. Owning the accept loop is what keeps the
       handler signature single. Mongoose is GPLv2/commercial — do not
       reach for it.
db     both drivers are compiled in and *which* database is used is read
       from DB_TYPE at startup, never decided by the build. -Dpostgres and
       -Dsqlite only decide what the binary could open; asking it for a
       database it has no driver for is SC_ERR_UNAVAILABLE, not a crash
db     there is deliberately no sc_db_query() both backends implement. The
       dialects differ, and a repository that has to know which one it is
       talking to should have to say so — the TypeScript path spells the
       same decision as a discriminated union. Statements go through
       sc_db_native(), in a file that already knows the dialect
pg     the driver is startup-only so far: sc_db_open blocks. The request
       path wants PQsocket / PQconsumeInput / PQisBusy on h2o's loop and
       nothing has written it yet
pg     libpq exposes no SQLSTATE for a *connection* failure, so PQping is
       what tells "not yet" from "not like this" — server there and
       refusing is permanent, nothing answering is worth a retry. The
       TypeScript path reads SQLSTATE classes 28, 3D and 42 for the same
       decision
sqlite WAL and foreign_keys are per *connection*, not per database. A
       connection that forgets them enforces no constraint the schema
       declares and serialises every reader against the writer
pg     Unix socket, not TCP loopback, when the database is on this host —
       83.4 to 48.1 µs for one connection string
pg     one round trip per request: user row and roles in one statement.
       A round trip costs more than the join it saves.
pg     do not hand-write row extraction. structs and from_row/bind_params
       are generated from contracts/db — 330 columns is not a review task.
       Query construction stays hand-written; that part is business logic.
jwt    require the claim before checking it. `exp` absent, or null, or a
       string, is not `exp` valid — see Architecture.md, Safety net. That
       is the verify path; the session hit path checks no claim at all and
       must not start, see section 5
zig    a host build needs addHostSystemPaths on every artifact that includes
       uv.h, a libpq header or anything of h2o's
zig    -Dtarget=x86_64-linux-gnu is NOT the native target as far as zig is
       concerned, even on that machine: it stops consulting /usr/include
       and /usr/lib, and h2o then fails with 78 lines of 'openssl/ssl.h'
       file not found. targetIsHost() is what tells the two apart, and it
       is why addHostSystemPaths adds those directories for a named host
       target and not for a native one, which already has them.
zig    on Debian and Ubuntu `zig libc` reports sys_include_dir=/usr/include
       while asm/errno.h sits one level deeper, under the multiarch triple.
       build.zig generates a corrected description and hands it to every
       target, because libsodium and libtsan fail without it and neither is
       a compilation this build declares.
zig    the cdb step writes compile_commands.json into the CURRENT directory.
       Run `zig build` from fast-servers/ or find a stray copy later.
zig    a dependency's artifact needs setLibCFile() of its own — it does
       not travel from the target that links it. libsodium went without
       for a while and nobody noticed, because a cached object is not
       recompiled: only changing the optimize mode asked for a fresh one
       and the build fell over. Check a new dependency with a mode this
       tree has not built yet, not with the one it has.
biome  `check --write --unsafe` deletes console calls rather than flagging
       them. It removed a diagnostic in tests/integration once, silently.
       Read the diff after running it, or do not pass --unsafe.
arnm   the json reader is a table and not a cursor — 0.7.5 took the
       cursor away. There is no getter per value and no call that says
       what a value is. One walk fills every field the table names and
       hands back a bit per field; a member of the wrong type stops that
       walk, and the fields after it stay unset. Decide from the mask and
       never from the walk's result: an absent member and one of the
       wrong type are then the same "not there", which is the reading a
       verifier wants, and neither can turn into a wrong answer about a
       different field.
arnm   a scalar handed out as a handle can never be read. The only two
       calls that take one are the object walk and the array read, so an
       array's string elements are out of reach — which is why jwt.c
       refuses a list-valued `aud` rather than walking it. A member that
       has to be read is a member a table converts where it stands.
sodium its SHA-256 is the portable C one — crypto_hash/sha256/ has only
       a `cp/` directory, where AEGIS has an aesni one. It runs at about
       0.43 GB/s where OpenSSL does 2.2. At JWT sizes that gap mostly
       disappears into per-call overhead, so swapping the library buys
       almost nothing; measure before believing otherwise.
h2o    the status line's reason phrase is res.reason, and it used to be
       "Error" for everything but 200 here while the other backend had a
       table. Invisible while the roles answered 200 and 404, a difference
       a client can see the moment a route answers 400. One table now, in
       http_common.c, and both backends read it.
h2o    a header added with sc_http_header_add is not copied by h2o: the
       value goes into the request pool, which outlives the handler, and
       the name has to be a literal. A stack buffer handed straight to
       h2o_add_header_by_str is a header value read after the frame is
       gone.
cors   what @elysiajs/cors puts on the wire is not all policy. Three of
       its headers are inert -- Allow-Headers on a response that is not a
       preflight, Expose-Headers filled with request header names, and the
       literal "undefined" when a preflight asked about no headers -- and
       backend/src/cors.c says so rather than reproducing them. AGENTS.md
       section 0: do not force artificial parity. What a browser acts on
       was compared request by request against the reference, not reasoned
       about.
h2o    a 204 must not describe a body it does not have. res.content_length
       is SIZE_MAX for "there is none" -- 0 makes h2o send Content-Length: 0
       -- and should_use_chunked_encoding refuses 204 anyway, so nothing
       falls back to chunked. The fallback backend writes the same status
       line and the same absent headers; sc_http_reply owns both.
pg     libpq's default notice processor writes to stderr, and this process
       writes one JSON object per line there. A `CREATE TABLE IF NOT EXISTS`
       that skipped an existing relation lands in the middle of the log
       stream and stops it parsing. PQsetNoticeProcessor at connect, and the
       notices are dropped: the event vocabulary is closed and there is no
       contracted event for "the database remarked on something".
js     valibot's maxLength and minLength count UTF-16 code units and its
       trim() removes U+00A0 and U+FEFF among others. A byte count refuses
       names the reference path accepts -- every second German surname is
       one unit and two bytes -- and sizes buffers against the wrong number.
       backend/src/field_rules.c is where that arithmetic lives, and its
       test compares against valibot's own answers rather than its source.
arnm   a scalar member handed out as ARNM_JSON_FIELD_TYPE_VALUE cannot be
       read, so "what type is this member" is answered by asking: a walk
       typed BOOL answers only for a bool, DOUBLE only for a number, and
       the two structural reads answer for an object and an array. What is
       left is null. user_routes.c needs it because valibot names the value
       in its refusal and a client is told the same sentence either way.
core   grdu_binary_to_base64 / grdu_binary_from_base64 are pinned to
       sodium_base64_VARIANT_ORIGINAL. A JWT is base64url without padding,
       so jwt.c calls sodium directly with the url-safe variant. The two
       alphabets differ in two characters, which is enough that a token
       fails only sometimes — roughly seven in ten carry a '-' or '_'.
```

Add to this list when something costs you an afternoon.

---

## 7. Where tests go

```text
<component>/tests/    unit tests, beside the component they test
tests/contract/       contracts/test-vectors/, run against this implementation
tests/integration/    the assembled binary, driven over sockets by bun test
```

Unit tests live beside their component rather than in one tree at the root, which is where
arnm and gradido-blockchain-core keep theirs. Those are one library each; this is five, and a
test binary that links one component and has only that component's include directory on its
search path is what proves a header carries its own dependencies. A shared test tree with all
five paths on it can never fail that way.

`tests/contract/` holds what tests neither a component nor the binary, but the **agreement**:
`contracts/test-vectors/<subject>.json` is read here and by
`packages/contract-tests/`, and each implementation is measured against the file rather than
against the other. `contracts/AGENTS.md`, *test-vectors*, is the shape; `vectors.hpp` is the
loader every subject shares.

Two rules, and the second is the one that is easy to get wrong:

- **A runner is not a unit test and does not live beside a component.** It needs arnm's header
  to read the vector file, and a component test that sees a header its component does not carry
  stops proving what the rule above exists to prove.
- **What a vector decides does not get a second answer in a unit test.** Which payloads are
  refused is shared behaviour; a copy of it in `service-core/tests/` is a second source of truth
  for exactly the question the contract exists to answer once. What belongs in the unit test is
  what a contract cannot express — null arguments, buffer bounds, a round trip through this
  implementation's own signer. `service-core/tests/test_jwt.cpp` is written against that line
  and says so at the top.

`tests/integration/` holds what tests the assembled binary rather than a component. The suite
runs against `http-probe` — once per HTTP backend, because the point is that both answer the
same — and its README lists where they do not.

Two rules about it:

- **A test route is still a route.** Nothing that exists so a test can observe something goes
  into backend or federation; it goes into the probe. `/_health` is operational, it is the only
  route the roles have, and it is not a precedent.
- **It is not a workspace.** `bun install` must not need anything under `fast-servers/`, or the
  fast path stops being droppable, so this directory is absent from the root `package.json` and
  has no dependencies of its own. `bun test` from inside it is the whole setup.

`bun clear` at the repository root removes what both builds leave behind here.
`scripts/clean-all.ts` names `fast-servers` explicitly, for the same reason: it is not a
workspace, so the loop that cleans those never arrives.
