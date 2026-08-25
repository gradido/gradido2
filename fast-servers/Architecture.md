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

- ASan, UBSan and TSan in CI, not only locally. TSan matters most — the shared session store
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

The C implementation holds one session store shared by all threads. This is the structure the
whole architecture rests on, so its invariants are written down rather than left to the code.

It is not a hash table. **The JWT carries the slot index, so a lookup is an array read.** Every
part below follows from that one decision: there is no key to mix, no probe walk, no collision,
no full-cache case that costs anything, and expiry is a comparison at a single known position
instead of work spread over every walk.

```text
one bucket_vector of session pointers, indexed directly by the token
one cursor           the oldest live entry; once full, also the next slot written
reference counting   unchanged, and it is what makes releasing a slot safe
```

### The token carries the slot

A hard timeout on a token that is never refreshed means sessions expire in creation order. The
JWT already carries `created_at` for that; it now also carries the index of the slot its session
was placed in. The read path is then:

```text
verify the signature                       <- everything after this is still hostile input
require created_at, require the slot index <- absent is not the same as zero
check created_at <= now + skew
check now - created_at < SESSION_HARD_TIMEOUT_MS
    => older than that: mint a new token and a fresh session, and never look the old one up
read the slot; empty or out of range => miss
take a reference
check the entry's user_id against the token's
    => mismatch: release, miss
```

The token is older than the timeout in exactly the case where its session is gone anyway, so the
refresh and the eviction are the same event seen from two sides. A client that comes back after
eleven minutes costs one insertion and no lookup; a client inside the window costs one bounds
check and one load.

**The index is attacker-supplied and it is trusted only because it is signed.** Nothing reads it
before the signature verifies, and it is range-checked afterwards regardless — a signed index is
protected against forgery, not against a bug that wrote the wrong one. *Safety net* says a claim
that is absent is not a claim that passed; here that rule has a second edge. A missing index must
be a miss, because the C default for a missing integer is zero and zero is a valid slot.

**The `user_id` check is not a formality, it covers the one case where a slot can be reused under
a live token.** Ordinarily it cannot: a slot is released only when its entry has timed out, and
that entry's `created_at` is the token's, so any token addressing a released slot was already
rejected two lines earlier. The exception is a store with no free slot left, where the oldest live
entry is overwritten early — see *The cursor*. Then a valid token can address a slot that now
holds someone else's session, and the `user_id` comparison is what turns that into a miss.

Comparing `created_at` as well makes the match exact rather than merely safe: the entry holds it
for the expiry check anyway, and with it a token can only ever find the session it was minted
with. Without it, two live sessions of the same user can alias — harmless, because a session is a
disposable working view (`AGENTS.md` section 7), but it costs one comparison not to have to argue
that.

### Two lock layers, and they must stay apart

```text
the store    which sessions exist       one reader/writer lock
a session    what is inside one         one main rwlock + one per data set
```

The primitive is `uv_rwlock_t`, from the platform layer above — `std::shared_mutex` would put
C++ on the request path, which `AGENTS.md` section 2 reserves for leaf modules.

They protect different things and are never held together. **The store lock is released
before any session lock is taken.** Holding one while taking the other couples two
structures that have nothing to do with each other and creates a deadlock across them.

The order within the lookup is not a matter of taste:

```text
take the store lock (shared)
    read the slot
    increment the reference count      <- inside the lock, not after it
release the store lock
    work, using only the session's own locks
```

Incrementing after the release is a use-after-free. In the gap another thread releases the
session, the count falls to zero, the memory goes back to the arena, and the increment then
lands in it. Holding the shared lock keeps the cursor walk out, because releasing a slot needs
the exclusive one — and a session still in the store has a count of at least one, so it cannot
be freed while it is being read.

Direct indexing makes this lock cheap enough to keep and small enough to remove later. It is held
for two instructions, and *Open* records what it would take to drop it entirely.

### The vector

```text
bucket_vector of pointers, chunks of a fixed size, chunks never move
capacity        sized for the sessions created within one SESSION_HARD_TIMEOUT_MS
8 bytes a slot  one arena for the sessions themselves
```

A bucket vector rather than a flat one, for a reason that is about concurrency and not about
allocation: growing a flat vector moves every element, and a reader holding a slot address across
that move reads freed memory. Chunks that never move make growth a published pointer to a new
chunk and nothing else — readers in existing chunks are untouched, and a reader whose index is
beyond the current size takes a miss, which is already a case the read path handles.

There is no hash here, and that removes a whole failure class rather than a line of code. *What
was measured* records two reproductions where a colliding key set does not make a bounded-probe
cache slow but makes it stop holding anything — correct answers, hit rate on the floor, nothing
in the log. An index that the server itself handed out cannot collide, cannot be strided, and
cannot be chosen by an attacker. The seeded splitmix mix and `CACHE_PROBE` stop being session
concerns; they remain the rule for any cache that still derives a slot from a key.

### The cursor

One cursor, and it is the only moving part of expiry. Because sessions are created in increasing
time order and every one of them dies exactly `SESSION_HARD_TIMEOUT_MS` after its creation, the
store is a ring in creation order: the live sessions occupy one contiguous run of it, the oldest
at the cursor and the newest at the end of the run. Expiry therefore needs no sweeper thread, no
walk of the whole store and no check on the read path. It needs one comparison, at one known
position, paid by whoever creates a session.

Creating a session:

```text
at the cursor, while the entry there has timed out:
    drop the store's reference    <- the session is freed if the count reaches zero
    clear the slot, advance the cursor
write the new session at the end of the live run, and extend the run by one
```

The walk keeps going for as long as it finds timed-out entries, so a burst of expiries goes back
to the arena at once rather than one insertion at a time; the cursor stops on the oldest entry
that is still live, which is where the next insertion will start looking. Nothing else moves it.
**Once the store is full the two positions are the same slot** — the entry the cursor stands on is
the one written a full lap ago, so checking whether the oldest has timed out and finding room for
the newest are one operation. That is the steady state, and it is the case the capacity is sized
for.

**Insertion never fails.** If the run has filled the ring and the entry at the cursor is still
inside its timeout, the new session takes that slot anyway: the store drops its reference, and a
session a request is still working on survives, owned by that request until it finishes. This is
the only path that can retire a session before its timeout, it is the reason the `user_id` check
exists on the read path, and it degrades as a cache miss rather than as an error. There is no
full-store case to handle and no reason to grow.

That makes the capacity a bound with a name: **the number of sessions created within one timeout
window.** Below it nothing is ever evicted early; above it the excess turns into misses in
creation order, which is what the oldest client's next request was going to cost anyway.

The walk is short, it is bounded by the number of entries that expired since the last insertion,
and it touches nothing but slots and reference counts — so it runs under the store's exclusive
lock without holding it across anything that can block. Freeing a session whose count reaches zero
returns arena memory and takes no other lock; *Two lock layers* is why that is allowed to happen
there at all.

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
the slot            is one reference
a request using it  is one reference
the cursor walk     clears the slot and gives up that reference
request end         gives up its reference
freed               exactly when the counter reaches zero, never anywhere else
```

Nothing is a special case. A session released while two requests are using it simply loses
the store's reference and lives on, owned by those requests; the last of them to finish
frees it. Timing out, being overwritten in a full store, and shutdown are the same operation
applied to one slot, one slot, and all of them.

**Reaching zero implies unreachable.** No slot points at a session whose count is zero, so no
lookup can revive it and the release path needs no lock — only `acq_rel` on the decrement, so
that the last user's writes are visible before the memory is reused. The increment may be
`relaxed`, but it must happen *inside* the shared lock: read the slot, release the lock, then
increment is the same race with extra steps.
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
through the pagination cursor, and read older windows from the database when someone actually
pages back. That is the difference between 545 sessions and 57 on the same machine.

The other lever is in `contracts/db/transactions.json`, recorded there as open: the two
denormalised `varchar(512)` names and four uuids per row are 116 of the 288 bytes. Holding
ids in the session and resolving names where they are rendered gives back another 40%.

The index itself does not appear in this arithmetic and that is the point of it. 545 sessions
at 8 bytes a slot are 4,4 KiB — one twentieth of a percent of what the sessions cost — so the
capacity is sized for the peak minute of a bad day and not for the memory. Where the earlier
designs had to weigh an index against the working set, this one has nothing to weigh.

### What was measured, and what it changes

`../../h20Test/bench_cache` measures the design this one replaced: an open-addressed table with
a hard timeout, a `uint16_t` slot index, and several ways to handle a full one. `rand` keys, one
Ryzen 7 5700G, single threaded, nanoseconds per lookup. It is kept because three of its results
are the reason the direct index is worth having, and because the numbers are the only defence
against re-deriving a hash table here later.

**Lazy expiry during the probe walk is not a shortcut, it is the reason to use open
addressing at all.** A map-based cache cannot reclaim in passing and has to sweep; at a
million live entries that is 1 730 ns per operation against 215 for the open-addressed one,
and the whole difference is the sweep. Behind a shared lock that is the difference between a
cache and a stall. The cursor is the end of that line of argument: reclaim in passing became
reclaim at one known position, and the walk it used to ride on is gone too.

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
The row that matters now is the last one: the cost of overfilling a hash-routed cache is
super-linear, where the cost of overfilling this one is a session evicted early, priced in
*The cursor*.

**The hash is the entire safety margin, and `CACHE_PROBE` is what makes that true.** A
bounded probe means a colliding key set does not make the cache slow, it makes the cache stop
holding anything — correct answers throughout, hit rate on the floor, nothing in the log. Two
such failures were reproduced: a masked table over an unmixed key dies on keys that stride by
the table size, and `std::unordered_map` under libc++ *is* that masked table, because
`reserve(65536)` yields a power-of-two bucket count indexed by masking. 19 ns became 25 µs.

Mix the key — splitmix64 finalizer, two multiplies — **and seed the mix once per process**.
The seed is what makes an adversarial key set unconstructible; it costs one XOR. **This is the
result the current design retires rather than applies**, and it is the strongest single reason
for the direct index: the failure it describes is silent, is reachable from outside, and cannot
occur at all once the server hands out the slot instead of computing it. The rule stands
wherever a slot is still derived from a key; the session store is no longer such a place.

The layout note from that work survives in a different form. A slot that holds an index into a
dense array of entries plus a fingerprint was worth having because it decoupled the table count
from the memory. Here the slot holds a pointer and there is no table count, so *The working set
needs a number* can size the capacity for the worst minute rather than for the megabytes.

### Where this design comes from

It is time routing, taken to the position it was always heading for. That design — one table per
minute, eleven of them, the table computed from the token rather than searched for — is recorded
below, because two of its arguments carry over unchanged and one of its corrections is the reason
this one is safe. `../../h20Test/bench_session_cache` measured **30 bulk reclaims where the
hash-routed equivalent did 386 260 inline ones**, and 8 to 21 % less time per request between
sixteen thousand and a quarter million live sessions.

The step from there to here is to stop rounding. If the token can name the minute, it can name
the session: one slot per session rather than one shard per minute, an array read rather than a
routed lookup, a comparison at the cursor rather than a pass over a shard, and no clock skew
inside the store at all. Everything time routing bought is bought here more cheaply — expiry off
the request path, no lock contention on the common path, no sweeper — and the parts of it that
were open stop being open:

```text
time routing                      direct index
one shard per minute              one slot per session
reclaim = walk a shard, 1/minute  reclaim = one comparison, at the cursor
11 index arrays, 2.75 MiB floor   one vector, 8 B a session
skew moves a token between shards skew is a bound in JWT validation, nothing more
refresh tokens end the design     a refresh mints a new slot, order is preserved
```

That last line is worth stating on its own. Time routing died the day gradido issued refresh
tokens, because a refreshed token would no longer sit in the minute its session was created in.
Here a refresh is a new session in a new slot, which is what the ten-minute rule already does
every time a client comes back late — the creation order the ring depends on is maintained by
construction rather than by the absence of a feature.

### What the shard design settled, and this one inherits

An earlier version of this document rejected time routing on the grounds that a shard cannot be
reclaimed in bulk while an entry in it still has a live reference. **That was wrong, and the
reference counting above is exactly why.** Dropping a shard is dropping the table's reference on
everything in it: the slot is cleared, the count falls by one, and a session a request is still
working on survives — owned by that request until it finishes. The second half of the old claim —
that a lazily growing working set cannot simply be cleared — confused two storages: the index
holds a pointer, the working set lives inside the session, and clearing an index never touches
it. Both corrections are load-bearing here: the cursor releasing a slot is the same operation the
shard reclaimer would have performed, applied to one entry at a time.

**Check `now - created_at >= SESSION_HARD_TIMEOUT_MS` against the token before the store is
touched at all.** Under the hash-routed cache this was merely cheap — an expired session never
reaching the table. Under time routing it became the first line of the safety argument. Under
direct indexing it is that and more: it is what guarantees that no valid token can address a slot
the cursor is about to release, which is the whole reason the read path can stay as short as it
is.

```text
check now - created_at >= SESSION_HARD_TIMEOUT_MS against the token, first
    => no valid token addresses a slot the cursor is releasing
    => no reader can still ENTER such a slot; only readers already inside it
    => what protects the read path is a grace period, not a lock
```

**The premise is `created_at`, and it has to be bounded on both sides.** A token from the past is
handled by the TTL check above. A token from the *future* — an issuer whose clock runs ahead —
outlives the slot it was given and can still be presented after the cursor has passed over it and
handed the slot to someone else. That is a bound in JWT validation (`created_at <= now + skew`),
not something the store can defend against, and it is why the `user_id` check on the read path is
mandatory rather than defensive. Compared with time routing, where skew silently moved a token
into the wrong shard, the failure is at least contained: one lookup, one mismatch, one miss.

### What it is worth at the size this targets

The `bench_session_cache` ladder starts at a thousand live sessions and time routing *loses*
there — 19.3 ns against 13.4 for the hash-routed table. *The working set needs a number* puts
this machine at 545 sessions in 8 MB, below the bottom of that ladder.

The direct index does not have that problem, and the reason is worth being precise about rather
than assuming: what cost time routing its nanoseconds at small sizes was the extra indirection
and the cold shard arrays, both of which are gone. A bounds check, a load and an atomic increment
do not have a size at which they lose. But the honest version is that at 545 sessions none of
this is what makes a request slow, and the reasons to take this design are the ones that do not
appear in a single-threaded nanosecond column at all:

- expiry leaves the request path entirely, and leaves the background too — there is no sweeper
  and no per-minute pass, only a comparison paid by whoever creates a session
- there is no hash, so the silent hit-rate collapse in *What was measured* is unreachable
- the store lock is held for two instructions and is a candidate for removal, which the probe
  walk never was
- the index costs 8 bytes a session, so the capacity is a policy decision rather than a budget one

### Open

**The grace period on the read path.** The safety argument above shows that no reader can enter a
slot the cursor is releasing, which leaves exactly one window: a reader whose token passed the TTL
check, and whose entry times out between that check and its increment. Three ways to close it,
in the order they should be reached for:

- the store rwlock, shared on lookup and exclusive for the cursor walk. Trivially correct, and
  cheap here in a way it was not before: the walk runs once per session creation, not on every
  miss. Its cost is the reader counter's cache line, which *What was measured* puts above eight
  cores — and this machine has four.
- release with a margin: let the cursor treat an entry as gone only at `created_at + timeout +
  grace`. It makes the window seconds wide at the cost of nothing on the read path, but it is an
  argument about scheduling, not a proof — a descheduled thread can outlast any margin.
- an epoch counter the reclaimer waits out. Costs nothing on the read path and is correct, and it
  is real concurrent code with real ways to get it wrong.

Take the lock now, and let a profiler rather than this section decide whether it is ever removed.

**Session working sets grow lazily while only a shared lock is held.** Allocating from the
session's arena at that moment races with the other readers, and this is unchanged by anything
above — the store's structure was never what made it hard. The options differ enough to matter:

- a per-session sub-arena, reserved with a cap when the session is created
- growth upgrades to the write lock
- per-session chunks with their own synchronisation

Until it is decided, nothing on the read path may allocate.
