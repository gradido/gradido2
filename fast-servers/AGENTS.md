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

## 2. C++ is for leaf modules only

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

Prefer no dependency at all. h2o, yyjson and libpq earn their place; a library that saves
fifty lines of C does not.

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
```

---

## 6. Known idioms

Record here what keeps being reinvented or mis-remembered, so the next agent does not
rediscover it:

```text
h2o   register the generator before the query goes out, not after —
      otherwise a client disconnect writes into a freed request
h2o   the request pool lives exactly as long as the request; anything
      the answer outlives it must not come from there
```

Add to this list when something costs you an afternoon.
