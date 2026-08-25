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

---

## Platform layer

`libuv` is the platform layer: threads and synchronisation now, filesystem, DNS and child
processes as they are needed. One dependency where there would otherwise be four `#ifdef
_WIN32` shims written at four different times.

It earns its place under `AGENTS.md` section 3a by what it bundles, not by any one part. For
threads alone it would not: 49 000 lines of C and 1.2 MB of `libuv.a` against roughly 150
lines of `pthread`/`SRWLOCK` shim. The decision is that everything it offers and this project
needs goes through it, so the platform seam exists once and is maintained once.

### The line that decides how it may be used

```text
loop-free    uv_thread_*  uv_mutex_*  uv_rwlock_*  uv_sem_*  uv_cond_*
             uv_barrier_*  uv_once  uv_key_*
loop-bound   uv_queue_work  uv_fs_*  uv_getaddrinfo  uv_spawn
             everything asynchronous
```

h2o runs on its own event loop (`evloop`, `H2O_USE_LIBUV=0`). The loop-free half of libuv does
not care and is usable exactly as it is — that is the half the session cache and the
background work need.

**The asynchronous half cannot be used from the request path without deciding which loop the
process has**, and there are two honest answers:

- build h2o against libuv (`H2O_USE_LIBUV=1`, `uv-binding.c.h`, 467 lines) so there is one
  loop; or
- keep `evloop` and run a `uv_loop_t` on its own thread for work that is not the request path.

Do not start a second loop on the request thread. Two loops on one thread is not a
configuration, it is a latency bug.

### Windows

libuv does not put this server on Windows and is not meant to. h2o's `lib/` contains no
`_WIN32` at all, `sys/un.h` in seven files and `fork` in two; its socket layer has three
backends — epoll, kqueue, poll — and all three answer *which sockets are ready*, while
Windows answers *which operation finished*. That is why libuv has a second complete set of
sources for `src/win/` rather than a fourth backend.

Windows is for the libraries — `shared-native`, anything embedded into a Node addon or a
desktop tool — and that is where libuv's portability is spent.

### If the server has to run on Windows anyway

Not at h2o's throughput, and not to serve load — to let someone develop, demo or test on a
Windows machine without the fast path being simply absent. The answer is not a second HTTP
library:

```text
libuv               already the platform layer, already decided
picohttpparser      already compiled into the h2o build today: 707 lines,
                    includes assert.h, stddef.h and string.h and nothing else
97 lines of glue    accept, parse, dispatch, write
```

Built and run: `x86_64-linux-gnu` 780 K and answering `curl`, `x86_64-windows-gnu` a 356 KiB
`.exe` importing only system DLLs — `WS2_32`, `KERNEL32`, the UCRT stubs, nothing to ship
beside it — plus both macOS targets. Neither dependency is new and neither adds a licence.

**The reason to write the ninety-seven lines rather than take a library is the seam.** A
second HTTP library brings its own routing, its own request lifetime and its own threading
model, and the handler code then forks in two — which is how a fallback stops being droppable
and starts being a second implementation. Owning the accept loop means the handler signature
stays the one h2o hands over, and the two servers differ in the loop and nowhere else.

What it deliberately is not: HTTP/1.1 with `Connection: close`, no keep-alive, no TLS, no
chunked bodies, no HTTP/2, one thread. `SO_REUSEPORT` has no Windows equivalent either, so
the multi-core story does not carry over. All of that is acceptable for *runnable* and none of
it is acceptable for *deployed*, which is the distinction this exists to draw.

If it ever has to be a library instead, the shortlist is one name: **civetweb**, MIT,
embeddable, C with a C++ wrapper. And one warning, because it is the top result for
"embeddable C http server" and it is a trap here: **Mongoose is GPLv2 or commercial.**
civetweb is its 2013 fork, from before that licence change. Check the licence before adopting
anything in this position — this repository moves money and cannot take a copyleft dependency
in the request path.

### Build

`allyourcodebase/libuv` carries a `build.zig`, so it is a dependency like any other:

```text
zig fetch --save=libuv "git+https://github.com/allyourcodebase/libuv.git#<commit>"
```

```zig
const uv = b.dependency("libuv", .{ .target = target, .optimize = optimize });
addMultiarchIncludeDir(b, uv.artifact("uv"), target);
addMultiarchIncludeDir(b, exe, target);
exe.linkLibrary(uv.artifact("uv"));
```

Verified building for `x86_64-linux-gnu`, `x86_64-macos`, `aarch64-macos`,
`x86_64-windows-gnu` and `aarch64-linux-musl`. Pin the commit, per `AGENTS.md` section 3a.

Both `addMultiarchIncludeDir` lines are required on Debian, and only for **native** builds: a
native build fails with `'asm/errno.h' file not found`, because `uv.h` includes
`linux/errno.h` and Debian keeps `asm/` under `/usr/include/x86_64-linux-gnu/`. It is needed
on libuv's own artifact *and* on every target that includes `uv.h`. Cross builds use zig's
bundled headers and never see it.

---

## Databases

```text
PostgreSQL   libpq, asynchronous on h2o's loop via PQsocket / PQconsumeInput / PQisBusy
SQLite       in process, called directly
```

Both are C calling a C library, and that is not a preference — it is what the measurements
leave room for.

### Where a query's time goes

Warm database, one connection, prepared statements. Method and full tables in
`../../h20Test/README.md`:

| | µs |
|---|---:|
| `SELECT 1`, TCP loopback | 32.8 |
| `SELECT 1`, Unix socket | 17.5 |
| the index lookup itself, user by unique key | 1.6 |
| the two queries of one uncached request, sequential, TCP | 83.4 |
| the same, Unix socket | 48.1 |
| the same, as one combined query, Unix socket | **37.3** |

Of those 48.1 µs, **3.6 are user-space CPU** — libpq's protocol handling, result parsing and
field extraction together. About 11 are system time; the rest is waiting for the server.

Two rules follow, and neither is about the driver:

- **Unix socket, never TCP loopback, when the database is on the same host.** 83.4 → 48.1 µs
  for one connection string.
- **One round trip per request.** User row and roles in one statement, not two: 48.1 → 37.3.
  Pipelining (`PQenterPipelineMode`) reaches the same 37.7 µs and costs more code.

That is 46 µs off an uncached request, against a *cached* request that costs 11.6 µs end to
end. Nothing else on this path is worth as much.

### Why the driver is neither rewritten nor moved to Zig

3.6 µs of user CPU out of 48 is the entire budget a different client could compete for. A
native protocol client would spend it differently and buy nothing measurable, while taking on
the wire protocol, TLS, SCRAM and failover that libpq already carries — and giving up the
integration with h2o's loop that already works.

SQLite settles itself the same way. It is a C library in this process; a binding in any
language emits the same call to the same `sqlite3_*` function.

### The mapping is generated, not written

The part that *is* expensive has nothing to do with speed. Counting the scope this path has
to reach — `../../gradido/database/src/queries` is the behavioural reference:

```text
96    exported query functions, 17 modules, 2 059 lines of TypeScript
330   columns across the 29 tables in contracts/db
30    distinct column types, including gradido_cent, uuid, timestamp_ms, bytes(32)
```

Those 2 059 lines **understate** the C work rather than describe it. TypeORM does the row →
object mapping for free, so almost none of them is mapping; they are query construction. C
has to write both. At two to three lines per column extracted — the null check, the fetch,
the conversion — the mapping alone is another two to three thousand lines, and it is the most
mechanical and least reviewable code in the repository: a wrong column index is a silent
wrong value, not a compile error.

So it is generated. **The source is `contracts/db/*.json`, which already carries `name`,
`type`, `nullable` and `identity` per column, is already normative for both implementations,
and already exists as a merge gate.** The generator emits, per table, a struct and the two
functions around it:

```text
contracts/db/users.json  ->  users.gen.h   struct user; column ids
                             users.gen.c   user_from_row(PGresult*, int, struct user*)
                                           user_bind_params(const struct user*, ...)
```

Three things follow, and they are why this is a generator rather than a clever language
feature:

- **One place decides how a type is read.** `gradido_cent` goes through `shared-native`
  because the generator says so once, not because 330 extraction sites were each reviewed.
  `../AGENTS.md`, *Amounts*, is then enforced rather than remembered.
- **The output is C.** It is greppable, it appears in a stack trace, and it needs no
  understanding of a metaprogramming facility to debug at three in the morning.
- **The same generator can emit the TypeScript side.** Two implementations that both derive
  their row shape from the contract cannot drift from it — which is what `contracts/` is for.

The generator itself belongs in TypeScript, run by the existing bun/turbo pipeline. It adds
no toolchain, and `contracts/AGENTS.md` already anticipates it: *"The `type` field is what
makes generated code possible."*

Four properties make it trustworthy, and they are the whole specification:

```text
deterministic   same input, byte-identical output. Sorted keys, fixed formatting,
                a banner naming the contract version and the hash of the input.
committed       the .gen.c and .gen.h files are in the tree and reviewed like any
                other code. A diff shows what a contract change did.
verified        CI regenerates and runs `git diff --exit-code`. That one line is
                the entire proof that the code still derives from the contract.
self-testing    the generator also emits the round trip — bind_params then
                from_row, compared — so the mapping is checked by construction
                rather than by 330 acts of attention.
```

**An AI is not this generator, and the reason is the third line.** A language model does not
produce byte-identical output twice, so CI cannot tell a contract change from a generation
wobble, and the merge gate stops meaning anything. The workload is also the shape where a
model is weakest: 330 columns of uniform, low-signal repetition, where a wrong index in row
217 is a silent wrong amount rather than a compile error — and `gradido_cent` must reach
`shared-native` every single time, not almost always.

Where the model belongs is one level up: **writing the generator, the type table and its
tests once**, and writing the query construction the generator deliberately does not cover.
Those are bounded, reviewable, judgement-shaped tasks. Emitting the same three hundred
functions by hand every time the schema moves is not.

**What it does not cover, and nothing would:** the query construction. Of the legacy modules,
55 sites build a `where` conditionally, 10 select relations by argument, and
`findUserByIdentifier` branches into three different queries depending on whether the
identifier parses as a uuid, an email or an alias. That is business logic wearing SQL, it is
written by hand in both languages, and it is the half worth spending review on.

### Why this is not a Zig comptime layer

`comptime` reflection over a struct is the other way to remove the same boilerplate, and it
is a good one — in a Zig program. Here it would have to declare the row structs in Zig, and
the C side needs those same structs, so either they are declared twice or Zig emits
C-visible `extern struct`s. At that point it is a code generator with a language dependency
in the data path, and it can only generate one of the two implementations.

`AGENTS.md` section 3 therefore stands, and for a better reason than caution: **zig builds,
it does not implement** — because the thing that needed generating had a better source than
a Zig type, and that source is the contract.

---

## Safety net

Non-negotiable wherever C runs, and more so where it was AI-generated:

- ASan, UBSan and TSan in CI, not only locally. TSan matters most — the shared session map
  is the one defect class expert review does not catch.
- Fuzzing for every parser that touches attacker-supplied bytes: JWT and JSON. The signature
  is verified before anything else is read; everything after it is hostile input.
- **A claim that is absent is not a claim that passed.** The verifier in `../../h20Test` checks
  `exp` only when the field is present and numeric, so a correctly signed token without `exp`,
  or with `"exp": null`, never expires — reproduced, all four cases accepted. Require the
  claim, then check it. The hard session timeout is only as hard as that one line.
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
the table    which sessions exist       one reader/writer lock
a session    what is inside one         one main rwlock + one per data set
```

The primitive is `uv_rwlock_t`, from the platform layer above — `std::shared_mutex` would put
C++ on the request path, which `AGENTS.md` section 2 reserves for leaf modules.

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

### What was measured, and what it changes

`../../h20Test/bench_cache` measures this structure directly: an open-addressed table with a
hard timeout, a `uint16_t` slot index, and several ways to handle a full one. `rand` keys,
one Ryzen 7 5700G, single threaded, nanoseconds per lookup. Three results bear on the design
above.

**Lazy expiry during the probe walk is not a shortcut, it is the reason to use open
addressing at all.** A map-based cache cannot reclaim in passing and has to sweep; at a
million live entries that is 1 730 ns per operation against 215 for the open-addressed one,
and the whole difference is the sweep. Behind a shared lock that is the difference between a
cache and a stall.

**"Several independent tables, not several locks over one table" is the right plan, and it
buys more than lock parallelism.** Sixteen tables chosen by the key's hash, each growing its
own overflow table:

| live entries, as % of one table | 25 % | 100 % | 400 % | 1600 % |
|---|---:|---:|---:|---:|
| one table, chained on overflow | 6.7 | 20.6 | 59.1 | 414 |
| sixteen, routed by the hash | **7.8** | **10.4** | **25.1** | **85** |

A chained table makes every lookup walk the whole chain; routing keeps the walk to one
sixteenth of it. Route with the **high** 32 bits of the hash and take the slot from the low
16 — the two must not overlap, which is the same mistake as deriving both from the low bits.

**The hash is the entire safety margin, and `CACHE_PROBE` is what makes that true.** A
bounded probe means a colliding key set does not make the cache slow, it makes the cache stop
holding anything — correct answers throughout, hit rate on the floor, nothing in the log. Two
such failures were reproduced: a masked table over an unmixed key dies on keys that stride by
the table size, and `std::unordered_map` under libc++ *is* that masked table, because
`reserve(65536)` yields a power-of-two bucket count indexed by masking. 19 ns became 25 µs.

Mix the key — splitmix64 finalizer, two multiplies — **and seed the mix once per process**.
The seed is what makes an adversarial key set unconstructible; it costs one XOR.

One layout note for when the table count is raised for lock parallelism: the slot can hold
the entry (24 B) or a `uint16_t` index into a dense array of the entries that exist plus a
16-bit fingerprint of the hash, so a probe step rejects a wrong candidate without
dereferencing. At four bytes a slot that is 4 MiB where inline entries are 24, faster on
misses, twice as fast on inserts — and two to four nanoseconds slower on a hit below one
table's worth. It decouples the table count from the memory, which is what makes "raise it to
sixty-four for the locks" a free decision rather than a 96 MiB one.

### Time routing, and what reference counting settles about it

A hard timeout on a token that is never refreshed means sessions expire in creation order,
and the JWT carries `created_at`. A cache indexed by *time* — one table per minute, eleven of
them, the table computed from the token rather than searched for — makes expiry stop being an
operation: reclaiming a minute is one bulk pass on a thread nobody waits for.
`../../h20Test/bench_session_cache` measures **30 bulk reclaims where the hash-routed
equivalent did 386 260 inline ones**, and 8 to 21 % less time per request between sixteen
thousand and a quarter million live sessions.

This section used to reject the idea on the grounds that a shard cannot be reclaimed in bulk
while an entry in it still has a live reference. **That was wrong, and the reference counting
above is exactly why.** Dropping a shard is dropping the table's reference on everything in
it: the slot is cleared, the count falls by one, and a session a request is still working on
survives — owned by that request until it finishes. That is not a new mechanism, it is the
one `sc_cache_destroy` already performs over the whole table; a minute-shard is the same loop
over one eleventh of it. The second half of the old claim — that a lazily growing working set
cannot simply be cleared — confused two storages: the shard holds a pointer, the working set
lives inside the session, and clearing an index never touches it.

So the idea composes. Three things about it do change once entries are reference counted, and
they are the reason this is recorded as a design to take rather than one already taken.

**Reclaim is a walk, not a `memset`.** The benchmark clears in O(1) because it stores entries
inline in a bucket vector; here a shard holds pointers, so the reclaimer walks its slots and
decrements. One pass over one shard, once a minute, off the request path — the 30-against-386 260
figure survives intact. What does not survive is the benchmark's memory column: half of that win
came from the dense inline storage, and sessions are separate allocations in either design.

**"Ten of eleven shards need no lock" is not free.** It is true that a finished shard is never
written — but a reader takes a pointer out of a slot and then increments a counter in it, and the
reclaimer is what can drive that counter to zero in between. This is the same race the table lock
exists for, in one place instead of everywhere. The argument that closes it is short, and it is
the one the eleventh shard was for:

```text
check now - created_at >= SESSION_HARD_TIMEOUT_MS against the token, first
    => no valid token addresses the shard being reclaimed
    => no reader can still ENTER it; only readers already inside it
    => the reclaimer needs a grace period, not a lock on the read path
```

**Check `now - created_at >= SESSION_HARD_TIMEOUT_MS` against the token before the table is
touched at all.** The JWT validation owes that check anyway. Under the hash-routed cache it was
merely cheap — an expired session never reaching the table. Under time routing it is the first
line of the safety argument above, which is a considerable promotion for a rule that costs one
subtraction.

The grace period is the open part, and the two candidates differ in cost: an epoch counter the
reclaimer waits out, or a per-shard rwlock taken shared by lookups and exclusive by the reclaimer
once a minute. The second is trivial to write and still writes a reader counter on every
lookup — the cache line that *Splitting it later* says starts to matter above eight cores,
except now spread over eleven counters rather than one. The first costs nothing on the read path
and is real concurrent code. Neither is decided here.

**The premise is `created_at`, and it has to be bounded on both sides.** A token from the past is
already handled: the TTL check above rejects it. A token from the *future* — an issuer whose clock
runs ahead — routes to a shard that currently holds a live minute and forces it to be reclaimed
early. That is a bound in JWT validation (`created_at <= now + skew`), not something the cache can
defend against, and it is the price of routing on a value the request supplies. It also means the
design ends the day gradido issues refresh tokens: sessions would stop expiring in creation order
and the whole premise with it.

### What it is worth at the size this targets

The benchmark's ladder starts at a thousand live sessions and time routing *loses* there —
19.3 ns against 13.4. *The working set needs a number* puts this machine at 545 sessions in 8 MB,
which is below the bottom of that ladder, and 11 index arrays sized as the benchmark sizes them
are 2.75 MiB of floor against those same 8 MB.

Neither figure is an argument against the design, and both are worth stating plainly rather than
being quoted later as one. `TABLE_CAP` is a benchmark constant; a shard sized for a peak minute's
creations rather than for 65536 slots removes the floor. And at 545 sessions the difference
between 13 ns and 19 ns is not what makes a request slow. The reasons to take time routing are
the two that do not appear in a single-threaded nanosecond column at all — expiry leaves the
request path entirely, and ten of eleven shards are read-only for a minute at a time — and both
of those only pay out under the threads this architecture is being built for.

### Open

Session working sets grow lazily while only a shared lock is held. Allocating from the table's
arena at that moment races with the other readers. This is the unresolved part of the
design, and the options differ enough to matter:

- a per-session sub-arena, reserved with a cap when the session is created
- growth upgrades to the write lock
- per-session chunks with their own synchronisation

Until it is decided, nothing on the read path may allocate.
