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
                   call answers with. Same pin and options as the core. It
                   was hostmem until arnm 0.5.0 renamed every symbol.
h2o                the fast HTTP backend, and the picohttpparser the other
                   backend compiles. Fetched by every build for that reason.
curl               libcurl, for service-core's mail client and for the
                   outbound HTTP this project will grow. Pinned at the last
                   commit of allyourcodebase/curl that still declares zig
                   0.15.1; everything after it wants 0.16.
openssl     lazy   h2o cannot be built without it -- <openssl/ssl.h> is in
                   h2o.h with no #ifdef around it -- and libcurl then has to
                   speak the same one. Pinned to the commit curl pins, not to
                   that package's HEAD: a different commit hashes differently
                   and would be a second OpenSSL in one process. Not fetched
                   on Windows, where h2o is off and curl gets Schannel.
zlib        lazy   h2o's gzip handler. Same pin as curl's, same reason.
libuv              the platform layer — see below. Every build links it.
googletest  lazy   the unit tests.
compile_commands   feeds compile_commands.json.
libpq       lazy   the PostgreSQL driver, built by allyourcodebase/libpq out
                   of a pinned postgres checkout — so that pin carries two,
                   and 5.18.4 is PostgreSQL 18.4. Requested with `ssl =
                   .None`, which is not a preference: the package pins a
                   different allyourcodebase/openssl than curl does, and two
                   commits of it hash differently, so asking for OpenSSL here
                   is asking for a second one in the process. What that gives
                   up is TLS *transport* to the database — scram-sha-256 still
                   authenticates. Lazy: the postgres checkout is 155 MB and
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

The loop-free half is used directly and not wrapped: `uv_rwlock_t` in the session cache,
`uv_mutex_t` in the log, `uv_thread_t` for the thread each role runs on, `uv_once` for the
cache's hash seed. There is no `service_core/thread.h` to go through, and there must not be one
again — a wrapper is the shim under another name.

What libuv does not offer stays ours: `service_core/atomic.h` is four functions over the
compiler's builtins, because libuv has no atomics and `<stdatomic.h>` is behind an experimental
switch on MSVC, which the CMake build has to compile.

**Fetch, do not vendor.** picohttpparser was a copy under `third_party/` before it was a
dependency, and the copy lost: two files nobody would ever diff against the original again are
worse than a download the Windows build does not compile. If something looks too small to be
worth pinning, that is an argument for not depending on it at all, not for copying it in.

**Watch what the core starts carrying.** This build pinned yyjson itself while blockchain-core
was at 0.16.0 and had no parser; 0.17.0 began linking libarnm, which carries one, and the pin
became two definitions of every `yyjson_*` symbol with link order deciding between them. When
the core takes on a dependency this build also names, one of the two has to go — and it is this
one, because a consumer that pins around its own library is pinning twice.

**Reach for arnm's surface, not for what is under it.** `arnm/json_reader.h` and
`arnm/json_writer.h` let no yyjson type, constant or include path through, so `jwt.c` names one
library where it used to name two and the parser underneath can be replaced without this
repository hearing about it. The same holds for the allocator and the conversions. Going around
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
has, and nothing about it was built to carry load. It is there so that everything around the
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
the token's TTL is checked BEFORE the store is touched — it is what guarantees that
    no valid token addresses a slot the cursor is releasing
a missing slot index in the token is a miss, never slot 0; the index is range-checked
    after the signature verifies, and the entry's user_id is compared to the token's
```

The store is not keyed and not hashed: the JWT carries the slot index and a lookup is an array
read. Do not reintroduce a hash here. `Architecture.md`, *What was measured*, has the two
reproductions of what that costs — a colliding key set does not make a bounded-probe cache slow,
it makes it stop holding anything, with correct answers, the hit rate on the floor and nothing in
the log. That failure is unreachable when the server hands out the slot; the seeded splitmix mix
remains the rule for any cache that still derives a slot from a key.

The store half of it is `service-core/src/cache.c`, and `service-core/tests/test_cache.cpp`
covers it. Run that under TSan and not only plain: its concurrent test proves little on its own,
because a reference count incremented outside the store lock does not fail an assertion. Two
things are still open — the grace period that protects the read path, and how a session's working
set grows while only a shared lock is held. Both are in `Architecture.md`, *Open*, and nothing in
the cache decides them.

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
       The fallback never sent one, so this was also a divergence the suite
       had not thought to assert. It does now.
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
       string, is not `exp` valid — see Architecture.md, Safety net
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
arnm   the json reader keeps its FIRST error and every getter after it
       answers empty. A getter on a value of the wrong type therefore
       silences the reads that follow, in another field entirely. Where a
       value may legitimately not be what is wanted — an element of an
       aud list, an optional member — ask arnm_json_reader_type_of() or
       _has() first: neither records anything.
sodium its SHA-256 is the portable C one — crypto_hash/sha256/ has only
       a `cp/` directory, where AEGIS has an aesni one. It runs at about
       0.43 GB/s where OpenSSL does 2.2. At JWT sizes that gap mostly
       disappears into per-call overhead, so swapping the library buys
       almost nothing; measure before believing otherwise.
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
tests/integration/    the assembled binary, driven over sockets by bun test
```

Unit tests live beside their component rather than in one tree at the root, which is where
arnm and gradido-blockchain-core keep theirs. Those are one library each; this is five, and a
test binary that links one component and has only that component's include directory on its
search path is what proves a header carries its own dependencies. A shared test tree with all
five paths on it can never fail that way.

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
