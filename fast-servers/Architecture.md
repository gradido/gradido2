# fast-servers Architecture

The C implementation of Gradido2 — h2o, the request path, the session cache, the database
drivers. It mirrors the domain structure of `packages/` and is allowed to lag behind it.

**Read `../Architecture.md` first.** It holds the system design that both implementations
share: why there are two, what a SessionContext is for, the consistency model, the DCI
organisation, the database and logging contracts. This file holds only what is specific to
building that in C, and it does not repeat the reasoning.

Two rules from there govern everything here, so they are worth restating:

- **No feature originates on this path.** Behavior that exists only in C silently removes
  itself from the fallback that keeps the project alive without its author.
- **This path must be droppable, not merely removable.** Running without it must require no
  code change anywhere else.

---

## Language boundaries

```text
C      the fast server: h2o, request path, session, repositories.
       Also shared-native. This is where most native code lives.
C++    leaf modules only, behind an extern "C" header:
       Justified by a library without a C equivalent
Rust   one module, dht-node, behind an extern "C" header.
       Justified by the same rule as C++ and by nothing else:
       libp2p has no C equivalent and writing one is not a module,
       it is a second project. See dht-node/Architecture.md.
zig    build system and cross compilation. No application code —
       its API still moves between versions.
```

The Rust module is the only one whose counterpart in `packages/` is a different
implementation rather than the same code seen from the other side. `../Architecture.md`,
*Peer discovery*, holds why, and what replaces the shared code: an interop test between the
two nodes, in CI, because no contract vector can express "these two find each other".

---

## Safety net

Non-negotiable wherever C runs, and more so where it was AI-generated:

- ASan, UBSan and TSan in CI, not only locally. TSan matters most — the shared session map
  is the one defect class expert review does not catch.
- Fuzzing for every parser that touches attacker-supplied bytes: JWT and JSON. The signature
  is verified before anything else is read; everything after it is hostile input.
- Contract vectors as a merge gate, green on both implementations.
- The Rust module is not exempt. Safe Rust ends at the `extern "C"` line: the pointers, the
  lengths and the lifetimes on the C side of `dht-node` are as unchecked as any other FFI
  seam, and they run under the same sanitizers. `#![forbid(unsafe_code)]` in the Rust
  interior, the `unsafe` confined to one file, and that file fuzzed like a parser — because
  what arrives there is a peer list built from what strangers on the network said.

---

## Session cache

The C implementation holds one session map shared by all threads. This is the structure the
whole architecture rests on, so its invariants are written down rather than left to the code.

### Two lock layers, and they must stay apart

```text
the table    which sessions exist       one shared_mutex
a session    what is inside one         one main shared_mutex + one per data set
```

They protect different things and are never held together. **The table lock is released
before any session lock is taken.** Holding one while taking the other couples two
structures that have nothing to do with each other and creates a deadlock across them.

The order within the lookup is not a matter of taste:

```text
take the table lock (shared)
    find the slot
    increment the reference count      <- inside the lock, not after it
release the table lock
    work, using only the session's own locks
```

Incrementing after the release is a use-after-free. In the gap another thread evicts the
session, the count falls to zero, the memory goes back to the arena, and the increment then
lands in it. Holding the shared lock keeps any evictor out, because eviction needs the
exclusive one — and a session still in the table has a count of at least one, so it cannot
be freed while it is being read.

### The table

```text
open addressing, power-of-two capacity, probe length CACHE_PROBE (default 8, configurable)
one shared_mutex for the table, one arena
```

The key hash selects the slot; the probe walks from there. The lock exists for a specific
race: a reader takes a pointer out of a slot, an evicting writer drops that pointer and
frees the session, and the reader then increments a counter in freed memory. Lookup and the
increment that follows it therefore happen inside the same shared lock.

One lock is enough at the sizes this targets. Readers do not block readers, and writes only
happen on a miss or an eviction, which are rare once the cache is warm.

**Splitting it later means several independent tables, not several locks over one table.**
Deriving the slot from the low bits of the hash and a lock from the high bits is broken —
two keys can land in the same slot while selecting different locks, and a probe run walks
past whatever boundary was drawn. If it is ever needed, the hash routes a key to one of N
tables, each with its own slots, its own lock and its own arena, and a probe stays inside
one by construction. Keep the routing line trivial so this stays an hour's work.

The reason to do it would not be threads waiting: a read lock still writes to the reader
counter, and that cache line bounces between cores on every lookup. It starts to show
somewhere above eight cores and never appears in a profiler as wait time. On a four-core
machine it is not worth having.

### Inside a session

One main `shared_mutex` plus one per data set — transactions, contributions,
transaction_links and so on — so that concurrent requests of the same user can work on
different sets at once. Three rules, all of them cheap now and painful to retrofit:

- **Fixed acquisition order.** Number the data sets and always take them ascending. Two
  interactions that need two sets in opposite orders deadlock, and that bug appears under
  load rather than in tests.
- **No lock upgrade.** `shared_mutex` has none: two readers both wanting to upgrade wait for
  each other forever. Release the shared lock, take the exclusive one, **and check again** —
  someone may have loaded the set in between.
- **Never hold a session lock across a database call.** Load outside the lock, then take the
  exclusive lock briefly to install the result. The lock is then held for a few
  instructions rather than for a round trip, which is also what makes the fine-grained locks
  affordable at all.

Two requests that load the same set at once is not an error — both read, the second
overwrites, the data is correct and the work was done twice. The frontend should not do it,
but the backend does not depend on it not happening.

### Reference counting

```text
the table pointer   is one reference
a request using it  is one reference
eviction            removes the pointer and gives up that reference
request end         gives up its reference
freed               exactly when the counter reaches zero, never anywhere else
```

Nothing is a special case. A session evicted while two requests are using it simply loses
the table's reference and lives on, owned by those requests; the last of them to finish
frees it.

**Reaching zero implies unreachable.** The table no longer points at a session whose count
is zero, so no lookup can revive it and the release path needs no lock — only `acq_rel` on
the decrement, so that the last user's writes are visible before the memory is reused. The
increment may be `relaxed`, but it must happen *inside* the shared lock: look up, release
the lock, then increment is the same race with extra steps.

### Insertion never fails

An occupied slot is overwritten. If the session sitting there is unused it is freed
immediately; if it is in use, only the pointer goes and the last user cleans up. There is
therefore no full-cache error to handle, and no reason to grow the table.

Among the CACHE_PROBE candidates, prefer in this order: free slot, expired entry, entry with
no current user, otherwise the first. Preferring an idle victim keeps sessions that are
actively serving requests inside the table, so less memory is held outside it.

### Lazy expiry

`SESSION_HARD_TIMEOUT_MS` is enforced during the probe walk, not by a sweeper thread.

A reader holds only a shared lock and therefore does not mutate the table: it treats an
expired entry as absent. The reclaim happens on the next `cache_put` that walks the same
slot — which is imminent, because the miss the reader just took is what triggers that put.
Expiry costs nothing beyond a comparison on a walk that happens anyway, and no background
work exists to schedule, starve or shut down.

### The working set needs a number

Computed from `contracts/db`, strings held as arena offsets:

```text
identity  (user + email contact + role)          414 B
a transaction row  208 B struct + ~80 B strings  288 B
```

Against 8 MB available for sessions on a 15 MB target machine:

| transactions per session | session | sessions in 8 MB |
|---|---|---|
| 25 (one page) | 8,0 KiB | 1026 |
| 50 | 15,0 KiB | 545 |
| 200 | 57,2 KiB | 143 |
| 500 | 141,6 KiB | 57 |

Unbounded, the ledger is the entire footprint: at 500 transactions the identity data and
every mutex in the session together are under half a percent of it. The bound is therefore
not a tuning parameter, it is the design.

Keep roughly two pages — `DEFAULT_PAGINATION_PAGE_SIZE` is 25, so about 50 — extend forward
through the cursor, and read older windows from the database when someone actually pages
back. That is the difference between 545 sessions and 57 on the same machine.

The other lever is in `contracts/db/transactions.json`, recorded there as open: the two
denormalised `varchar(512)` names and four uuids per row are 116 of the 288 bytes. Holding
ids in the session and resolving names where they are rendered gives back another 40%.

### Open

Session working sets grow lazily while only a shared lock is held. Allocating from the table's
arena at that moment races with the other readers. This is the unresolved part of the
design, and the options differ enough to matter:

- a per-session sub-arena, reserved with a cap when the session is created
- growth upgrades to the write lock
- per-session chunks with their own synchronisation

Until it is decided, nothing on the read path may allocate.
