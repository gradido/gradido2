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

---

## 3. zig builds, it does not implement

Build system and cross compiler. No application code — its API still moves between versions,
and the repositories currently disagree about which one is pinned (see `../AGENTS.md`,
*Toolchain*). Verify before assuming.

---

## 3a. Dependencies

Same policy as `../AGENTS.md` section 13, and it already holds here: `build.zig.zon` pins
every dependency to a fixed commit with a hash. Keep it that way — a floating dependency in
a C build is a floating dependency in the process that signs transactions.

Prefer no dependency at all. h2o, yyjson, libpq and libuv earn their place; a library that
saves fifty lines of C does not.

`libuv` is the platform layer — threads and synchronisation now, filesystem, DNS and child
processes as they are needed. It earns its place by what it bundles, not by any one part: for
threads alone it would be 49 000 lines against a 150-line `#ifdef` shim. Two rules come with
it, and `Architecture.md`, *Platform layer*, holds the reasoning:

```text
loop-free  uv_thread_*, uv_mutex_*, uv_rwlock_*, uv_cond_*, uv_sem_*, uv_once, uv_key_*
           usable as they are, next to h2o's own evloop
loop-bound everything asynchronous — uv_fs_*, uv_getaddrinfo, uv_spawn, uv_queue_work
           needs a uv_loop_t, and the process has h2o's. Do not start a second one on
           the request thread; put it on a thread of its own or change h2o's backend.
```

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

---

## 5. Before you touch the session cache

Read `Architecture.md`, *Session cache*, in full. The invariants there are not style
preferences and each of them was a bug first:

```text
the table pointer is a reference; freed exactly when the count reaches zero
the reference count is incremented INSIDE the table lock, never after releasing it
the table lock is released before any session lock is taken
data-set locks are acquired in a fixed order
no lock upgrade — release shared, take exclusive, check again
no session lock is ever held across a database call
the key hash is mixed AND seeded per process; the slot comes from the low bits,
    a table from the high ones, and the two never overlap
```

The seeded mix is not paranoia. With a bounded probe walk a colliding key set does not make
the cache slow, it makes it stop holding anything — correct answers, hit rate on the floor,
nothing in the log. `Architecture.md`, *What was measured*, has the two reproductions.

---

## 6. Known idioms

Record here what keeps being reinvented or mis-remembered, so the next agent does not
rediscover it:

```text
h2o   register the generator before the query goes out, not after —
      otherwise a client disconnect writes into a freed request
h2o   the request pool lives exactly as long as the request; anything
      the answer outlives it must not come from there
pg    Unix socket, not TCP loopback, when the database is on this host —
      83.4 to 48.1 µs for one connection string
pg    one round trip per request: user row and roles in one statement.
      A round trip costs more than the join it saves.
pg    do not hand-write row extraction. structs and from_row/bind_params
      are generated from contracts/db — 330 columns is not a review task.
      Query construction stays hand-written; that part is business logic.
jwt   require the claim before checking it. `exp` absent, or null, or a
      string, is not `exp` valid — see Architecture.md, Safety net
zig   a native Debian build needs addMultiarchIncludeDir on every artifact
      that includes uv.h or a libpq header; cross builds never hit it
http  the Windows fallback is libuv + picohttpparser + ~100 lines, not a
      second HTTP library. Owning the accept loop is what keeps the handler
      signature single. Mongoose is GPLv2/commercial — do not reach for it.
```

Add to this list when something costs you an afternoon.
