# fast-servers Architecture

The C implementation of Gradido2 — h2o, the request path, the session cache, the database
drivers. It mirrors the domain structure of `packages/` and is allowed to lag behind it.

**Read `../Architecture.md` first.** It holds the system design that both implementations
share: why there are two, what a SessionContext is for, the consistency model, the DCI
organisation, the database and logging contracts. This file holds only what is specific to
building that in C, and it does not repeat the reasoning.

Three rules from there govern everything here, so they are worth restating:

- **No feature originates on this path.** Behavior that exists only in C silently removes
  itself from the fallback that keeps the project alive without its author.
- **This path must be droppable, not merely removable.** Running without it must require no
  code change anywhere else.
- **A deployment runs this path or the TypeScript one, never both.** Nothing proxies between
  them: a route not implemented here answers `ROUTE_NOT_IMPLEMENTED` rather than reaching
  TypeScript. Lagging behind means a smaller route set, not a mixed deployment — the session
  cache lives in one process and cannot be shared across the two. See *One implementation per
  deployment* there.

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

## Threading

One model for the process, because a threading decision taken twice is a threading decision
taken differently. Three modules have already arrived at the same shape without consulting each
other — `dht-node` owns a thread and answers through a `drain` that never waits, the mailer owns
threads and is reached through a queue, the session cache is shared and holds its locks for two
instructions. This section is that shape written down, so that the fourth module does not have
to rediscover it and the fifth does not get it slightly wrong.

### The rule

```text
Does the waiting have a file descriptor?

  yes ->  it belongs on the event loop. No thread, no handoff, no context switch.
  no  ->  it belongs on a thread of its own, and how many of those there are comes
          from the resource, never from the number of cores and never from how long
          the wait is.
```

**Work does not leave the request path because it waits. It leaves because it serialises or
because it syncs.** Waiting is what an event loop is for. A lock only one thread may hold and an
`fsync` that stops a core are not, and they are the only two reasons a thread exists anywhere in
this document.

That is why the numbers in *Where a query's time goes* do not lead where they appear to. Of the
48.1 µs a query costs, 3.6 are user CPU and most of the rest is waiting for the server — and the
answer to that is not more threads to cover the wait, it is `PQsocket` on the loop, where the
wait costs no thread at all.

### The map

```text
main thread        config, log, sodium, signal handlers, then it joins and nothing else
role thread        one per selected role -- src/main.c, and see One binary, three roles
  loop threads     an HTTP role scales here: N h2o contexts, N evloops, SO_REUSEPORT
mail workers       1..worker_max, shared, queue-fed. See Mail
sqlite writer      one, shared, queue-fed. See Databases
dht-node           tokio, inside the Rust module, reached only through drain
```

Everything below the role line is a consequence of the rule and not a preference, and the two
entries that are not built yet are marked in their own sections rather than dated here.

### h2o is thread-per-loop, and that is the whole reason for the rule

```text
h2o_globalconf_t   once, read-only after setup, shared by every thread
h2o_context_t      one per thread, each with its own h2o_evloop_create()
h2o_req_t          belongs to one loop thread. No lock, and no handing it to another.
```

A blocking call in a handler therefore does not block **a request**, it blocks **a loop** — and
with it every other connection the kernel gave that loop, including all the ones that would have
been answered from the session cache in 11.6 µs. At eight loops and ten thousand connections, one
5 ms `fsync` stops about twelve hundred of them. That does not appear in a throughput average and
it is the only thing visible in a p99.

This is the difference from a thread-per-request server, and it is what makes the familiar
reasoning wrong here: *the operations wait on I/O, so run more threads than cores and let the
scheduler cover the wait*. That is sound where a thread carries one request. Where a thread
carries a loop it buys a stalled loop instead of a stalled request, and it costs a second thing
besides — an oversubscribed loop is descheduled with events pending, so its timers fire late.
Keep-alive expiry, request timeouts and the session store's expiry walk are all timers.

**Threads are the number of cores. There is no allowance for I/O wait, because there is no I/O
wait left on a thread once the rule above has been followed.**

`SERVER_THREADS` is the knob and its default is one loop per core. Two details of how it is
built are worth knowing before changing it: every listening socket is bound on the calling
thread before any of them serves, so a port already taken is one error line at startup instead
of N from N threads; and loop 0 runs on the role's own thread rather than on a thread of its
own, so a role still owns exactly the thread `main` gave it and `sc_http_run` joins the rest
before it returns.

The mailer is the one entry in the map that can push the process past that count, and it is why
`worker_max` is expressed in cores at all — see *The workers*. Against a remote relay a mail
worker is idle 99.9 % of the time, so it is not competing for a core; against a local one the
second worker is never started, because the first is never slow enough to ask for it.

### How a thread talks back to a loop

`service_core/http.h` carries three calls and both backends implement them:

```text
sc_http_defer(server, req, work, &ticket)   the handler, on its own loop
sc_http_resume(server, ticket)              the worker, from any thread
the resume callback                         registered once, runs on the owning loop
```

Underneath, h2o has `h2o_multithread_create_queue` / `_register_receiver` / `_send_message`,
and the fallback has a `uv_async_t`. h2o's is cheap enough not to think about: the write to the
wakeup fd is skipped when the receiver already has messages pending, so a busy loop takes no
syscall per message. The fallback's async coalesces and cannot say how many resumes it stands
for, so delivery there walks the table -- which is affordable exactly because that backend is
the one not carrying load.

Two things it does not do, and both have bitten every project that has tried this:

- **The request may be gone.** A client that disconnects takes its `h2o_req_t` with it, so no
  worker may hold that pointer across a thread boundary. What crosses is an id that the loop
  thread resolves back to a request or fails to resolve — see *The write must be answered, not
  acknowledged*, which is where that matters most. On the h2o side that id is backed by a
  generator registered at defer time: h2o clears the generator on a final send and calls its
  `stop` on a request it disposes, so `stop` fires exactly when the answer did not.
- **The reply goes back to the loop it came from**, not to any loop. The receiver is per-context,
  so which one to send to is part of what the worker was handed.

Per-thread state on an HTTP role hangs off the handler, not off a global: `on_context_init` /
`on_context_dispose` and `h2o_context_get_handler_context()`. That is where a per-loop SQLite read
connection lives.

### What this means for each kind of work

```text
CPU, request-shaped     the request thread. Routing, session lookup, JSON, domain logic.
PostgreSQL              the loop, asynchronously. PQsocket / PQconsumeInput / PQisBusy.
SQLite reads            the request thread, one connection per loop thread.
SQLite writes           one dedicated writer thread, queue-fed.
Mail                    the mail workers. Never the request thread, at any queue depth.
Peer discovery          the Rust module's own thread, drained, never waited on.
```

**What is worth controlling separately is the number of connections, not the number of threads.**
This is the whole case against a database thread pool. Postgres has a `max_connections` and its
own best concurrency, which has nothing to do with how many cores serve HTTP — and asynchronous
libpq lets those two numbers be set independently, because a connection is an fd and not a thread.
A pool ties them back together: a blocking connection needs a thread to block, so the connection
count becomes the thread count and neither can be tuned without moving the other.

The `fsync` is the exception the rule was written for, and it is the next section.

### SQLite: read where you are, write on one thread

SQLite has no socket and no asynchronous API, so it is the one place where the rule's second
branch applies inside the request path — and the answer differs between reading and writing,
because SQLite's own concurrency does.

**Reads stay on the request thread.** A read from a warm page cache is single-digit microseconds;
*Where a query's time goes* puts the index lookup itself at 1.6 µs. A context switch with a futex
wakeup costs several, plus what it does to both caches. Handing a read to another thread spends
more than the read.

What that requires is one connection per loop thread, opened `SQLITE_OPEN_NOMUTEX`. Today
`db_sqlite.c` opens one handle `SQLITE_OPEN_FULLMUTEX` and shares it, which is correct and is
also the ceiling: every statement serialises on that handle's mutex, so WAL's readers-and-a-writer
holds at the file level and is given back inside the process. One connection per thread removes
the mutex rather than contending on it, because no connection is then seen by two threads.

**Writes go to a single dedicated writer thread** holding the one write connection, fed by a
queue, answering through the loop's receiver. Four reasons, and the last is the one that pays:

```text
1  SQLite permits one writer regardless. N request threads that write serialise
   anyway -- the choice is only whether they serialise on their loops or off them.
2  busy_timeout stops being a question. Five seconds (db_sqlite.c) is the right
   number for role threads sharing a handle, and it is also five seconds of stopped
   event loop. With one writing thread there is no contention to wait out.
3  fsync never belongs on a loop. WAL with synchronous = NORMAL costs tens of
   microseconds, FULL costs tenths of milliseconds to milliseconds, and which one a
   deployment chose must not decide whether twelve hundred connections stall.
4  Group commit. One thread that can see several queued writes puts them in one
   transaction, so one fsync serves all of them. This is the largest throughput gain
   available on this path and it exists only when one thread sees the writes.
```

It is the mailer's shape with a different ceiling, and deliberately so: *The workers* says a
worker is a connection and grows the count to what the relay rewards. Here the count is one,
because that is what the database permits — same structure, and the resource sets the number in
both cases, which is the rule.

### The write must be answered, not acknowledged

A request that writes cannot be answered before the write is known to have happened. That is not
a latency preference, it is what the caller is asking: this repository moves money, and a `200`
that means *queued* is a `200` that lies about the one thing the client needed to know.

So the handler does not answer. It returns 0 without calling `h2o_send` — which is how h2o is
told the request has been taken over rather than declined — the request stays alive on its loop,
and the reply is written from the receiver callback once the commit has returned. Six things
follow, and they are the design rather than the implementation of it.

**The unit of the queue is a transaction, not a statement.** Whatever has to be atomic for the
domain is one queue item, because the writer batches items and an item is the smallest thing it
can keep whole. Two statements pushed separately may end up in the same batch, but nothing
guarantees it and nothing may depend on it.

**Group commit does not cost the answer its latency — under load it is what saves it.**

```text
one commit per request   request N waits behind the N-1 fsyncs queued ahead of it,
                         because SQLite lets one writer commit at a time regardless
group commit             request N waits for at most two: the batch already in
                         flight when it arrived, and the batch it lands in
```

That is the argument for the shape and not merely for its throughput. It also decides the
durability setting: `synchronous = FULL` is what makes *committed* mean *survives the power
going out*, it costs an `fsync` per commit — and group commit is what makes one commit serve a
batch, which is what makes FULL affordable. NORMAL is cheaper and answers a question nobody
asked here: it survives a crashing process and not a losing power supply.

**One item's failure must not become the batch's.** Each item runs inside its own `SAVEPOINT`,
so a constraint violation rolls back to that savepoint — no `fsync`, no effect on its
neighbours — and that item alone is answered with its error. The exception is honest and has to
be handled: an I/O error or a full disk takes the whole transaction with it, and then every item
in the batch is answered as failed, which is correct, because none of them happened.
`sqlite3_get_autocommit()` is how the writer tells the two apart rather than guessing from the
result code.

**A full queue answers; it does not wait.** The loop may not block on a queue, so a writer that
cannot take the item answers the request — `503` — the same way the mailer hands
`SC_ERR_QUEUE_FULL` back to its caller rather than deciding for it. See *What the queue costs,
exactly*: the ceiling is a number chosen at startup and never an allocation under load.

**A client that leaves does not cancel the write.** A read whose requester is gone may be
dropped; a write may not, because the request to move money was made and answering nobody is
not the same as not doing it. The disconnect suppresses the reply and nothing else.

**No `h2o_req_t` crosses a thread.** The queue item carries the originating loop's receiver and
an id; the loop thread resolves that id back to an in-flight request, or finds that it is gone.
That is the session cache's trick used again — an index and a check rather than a pointer, see
*The token carries the slot* — and it is what removes the lifetime problem instead of
synchronising it. The writer never holds anything that a disconnect can free.

The ticket is three numbers — loop, slot, generation — and the slot's generation and phase live
in one word, so that "is this ticket still what the slot holds" and "claim it" are a single
compare-and-swap. Asking in two steps is a race: a slot released and re-armed in between passes
both checks while belonging to somebody else. `service-core/src/http_defer.h` holds the rest.

**Noticing that the client left is not guaranteed and must not be needed.** h2o stops reading a
connection while a response is pending, so a client that closes after its request is often not
seen until the write fails; the fallback keeps reading and sees the EOF. Both are correct, and
the difference is asserted in `tests/integration`. What the design rests on is the other half:
the request cannot be disposed while a worker holds its ticket, so a resume that arrives at
nobody is delivered with a null request rather than into freed memory.

### Where the model is still open

- **Who owns the mailer and the database handles.** No role opens either yet — *What is built,
  and what is not*. When one does, the question is process-wide or per-role, and the answer is
  process-wide for both: two roles in one process talking to one relay and one database file
  should hold one queue and one writer between them, not two of each competing.
- **Nothing here is in CI yet.** The integration suite drives both backends over four loops and
  both come back clean under TSan and UBSan, which is the check being described — it is run by
  hand today and *Safety net* already says where it belongs.
- **The session cache is not yet the one this document describes.** `service_core/cache.h` is an
  open-addressing table with a hashed key, and *Session cache* above specifies direct indexing
  from the token with no hash at all. Nothing consumes either yet, so the design is ahead of the
  code rather than in conflict with it — but the multi-loop server is what makes it matter, and
  the store is shared across those loops rather than one per thread.

---

## Databases

```text
PostgreSQL   libpq, asynchronous on h2o's loop via PQsocket / PQconsumeInput / PQisBusy
SQLite       in process, called directly
```

Both are C calling a C library, and that is not a preference — it is what the measurements
leave room for.

### What is built, and what is not

`service-core/src/db*.c` behind `service_core/db.h`. Both drivers are compiled into every
default build, because *which* database a community runs is read from `DB_TYPE` at startup and
is not a property of the binary — `-Dpostgres=false` and `-Dsqlite=false` exist for a
deployment that knows it will never see one of them, and asking such a build for that database
is an `SC_ERR_UNAVAILABLE` naming the option, not a crash.

```text
libpq      allyourcodebase/libpq, out of a pinned postgres checkout. Not on Windows:
           that package compiles postgres' posix src/port. `-Dpostgres=true` there
           stops the build saying so, the way `-Dh2o=true` does
sqlite     sqlite.org's amalgamation, compiled by build.zig. Every target
```

What the surface carries is opening, probing, waiting and closing — the questions that are
genuinely the same asked twice — plus the environment they are configured from, which is the
TypeScript path's `DB_TYPE`, `DB_HOST`, `DB_PORT`, `DB_USER`, `DB_PASSWORD`, `DB_DATABASE`,
`DB_FILE`, down to the defaults. Waiting is what a database and its server starting together
need, and it turns on one distinction: a connection that was refused, unresolvable or timed out
is tried again, while a server that answered and *refused* ends the startup at once. libpq
publishes no SQLSTATE for a connection failure, so `PQping` stands in for the SQLSTATE classes
the TypeScript path reads.

Two things are deliberately absent, and each has a reason rather than a date:

```text
no query surface     the dialects differ; a repository that has to know which one it is
                     talking to should have to say so. Statements are written against the
                     driver, reached through sc_db_native() — see *The mapping is generated*.
no async path        sc_db_open blocks, which is right for a startup and wrong for a request.
                     PQsocket / PQconsumeInput / PQisBusy on h2o's loop is a second entry
                     point beside it, and arrives with the first repository that needs it.
```

TLS to the database is not on that list: libpq is built with `ssl = .LibreSSL`, the same
package h2o is built against, so a database on another machine is reached over an encrypted
connection. The Unix socket this section prescribes needs none of it.

Nothing opens a connection yet: no role reads a row, and a server that refuses to start over a
database it never queries would be a regression dressed as progress. `--version` reports which
drivers are in.

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
  claim, then check it. The hard session timeout is only as hard as that one line — and it is
  checked on the verify path, because the hit path checks no claim at all. See *Session cache*
  for why it does not have to.
- Contract vectors as a merge gate, green on both implementations.
- The Rust module is not exempt. Safe Rust ends at the `extern "C"` line: the pointers, the
  lengths and the lifetimes on the C side of `dht-node` are as unchecked as any other FFI
  seam, and they run under the same sanitizers. `#![forbid(unsafe_code)]` in the Rust
  interior, the `unsafe` confined to one file, and that file fuzzed like a parser — because
  what arrives there is a peer list built from what strangers on the network said.

---

## Mail

Sending a mail is one of the few things a request can trigger that reaches outside the process,
and the measurements say it costs either 85 µs or 100 ms depending on decisions that have very
little to do with the mail. The h2o prototype's `smtp_client` measured the difference; this
section is what came of it.

### libcurl, and why not two hundred lines of socket code

SMTP is a line protocol and a client for it is a weekend. It is still not what is here, for two
reasons that only show up later. The first is TLS: STARTTLS and SMTPS mean certificate
verification, hostname checking and a handshake, and none of that is a weekend. The second is
that the same library dials the outbound HTTP this project will need — a server that already
carries one HTTP implementation has no business growing a second client stack beside it.

What it costs is a dependency that arrives with opinions. Every protocol but SMTP and HTTP is
switched off in `build.zig`, along with the four system libraries curl looks for by default, and
the reasoning is in the comment there rather than repeated here.

### TLS: LibreSSL for the server, mbedtls for the client

h2o cannot be built without an OpenSSL *API*. `<openssl/ssl.h>` sits in `h2o.h` and
`h2o/socket.h` with no `#ifdef` around it, `st_h2o_socket_t` carries the SSL pointer as its
second field, and `lib/common/socket.c` branches on it 238 times — TLS is *in* the socket rather
than a layer over it. There is no `H2O_NO_SSL`, and the two macros with OpenSSL in the name
switch a callback and a deprecation warning.

That API is LibreSSL's, and the reason is arm64: `allyourcodebase/openssl` builds for x86_64
only. It ships pre-generated x86_64 assembly, adds it under `.x86_64 => ...` with `else => {}`
beside it, and compiles `crypto/bn/asm/x86_64-gcc.c` whatever the target is, so an aarch64 build
stops with 23 errors inside it — natively as much as cross, because the choice is made on
`cpu.arch`. h2o is portable C and compiles for aarch64 without a complaint; only the TLS package
stood in the way, and arm64 is what a small server is now.

LibreSSL is the same API kept portable, and h2o knows the fork: `socket.c` branches on
`LIBRESSL_VERSION_NUMBER` in four places. `allyourcodebase/libpq` pins the same commit, hash
included, so the database driver and the HTTP server are one package in the build graph and the
process holds one library rather than two that both export `SSL_new`.

libcurl is the exception, and a temporary one. Its package cannot be handed a LibreSSL — where
it chooses a backend, its `build.zig` reads `// TODO BoringSSL, AWS-LC, LibreSSL, and quictls` —
so the mail client speaks **mbedtls**, which curl pins itself. Two libraries with the *same*
symbols would be a link-order lottery; `mbedtls_*` is not `SSL_*`, so these two coexist. It is
still one library more than this project wants to keep patched. **When curl's package grows
LibreSSL, move libcurl onto it and this section loses a paragraph.**

Windows gets Schannel and neither of them. h2o has no Windows port, so that build serves over
the fallback backend, and libpq is not built there either — the mail client works, which is the
point of the arrangement.

### The connection is held; the socket is curl's business

| | per mail |
|---|---:|
| kept connection, sink on loopback | 85 µs |
| new connection per mail | 225 µs |
| kept connection, MailDev | 864 µs |
| new connection per mail, MailDev | 102 800 µs |

MailDev's 100 ms is a `setTimeout` before its greeting — `smtp-connection.js`, commented "Keep
a small delay for detecting early talkers". Anything measured against it measures that timer,
which is worth knowing before someone concludes from a staging environment that mail is slow.

One `CURL *` lives as long as the worker that owns it, and nothing in this codebase reconnects. curl replaces
a connection idle for two minutes (`conn_max_idle_ms = 118 * 1000`), and rejects one that is
dead or has unread input pending — which is what a `421` goodbye looks like — before reusing it.
A mail after an hour of silence therefore arrives, over a connection nobody asked for. The
handle is permanent, the connection is not, and the difference is invisible from the outside.

### The workers, and why not libuv's thread pool

One worker is permanent and sleeps on a condition variable when there is nothing to do. Others
are started only when the first cannot keep up — when it has been busy without a pause for
`spawn_after_ms` and `spawn_backlog` mails are still waiting — and retire after `linger_ms` of
idleness. The ceiling is half the machine's cores minus one, never below one.

A worker *is* a connection. A `CURL *` belongs to one thread, so "another worker" and "another
session to the relay" are the same act, and the pool shrinking again is what gives a relay its
connection slots back. That matters against a remote relay and nowhere else: at 30 ms RTT a
connection is idle 99.9 % of the time and mails per second scale linearly with how many are open
— 8/s at one, 127/s at sixteen — while against a local relay one worker is already worth 11 000
mails/s and the second buys nothing. Which is why they are not started up front.

The decision to grow is asked in two places, and the second one is the one that was missing at
first: after a push, and again each time a worker picks up its next mail. A producer that drops
two hundred mails at once and then goes quiet satisfies the backlog half immediately and the
"busy a while" half never — at the moment of the last push nobody has been busy long enough yet,
and nothing would ask again. Growth has to be something the workers notice while they work.

**Not libuv's thread pool**, although it is the obvious answer. `uv_queue_work` needs a
`uv_loop_t`, and on the h2o path this process has none: h2o runs its own event loop
(`H2O_USE_LIBUV=0`) and the only `uv_loop_t` in the tree belongs to the fallback HTTP backend,
which a given build may not even compile. Coupling the mailer to a backend that is a build option
is worse than owning two threads. AGENTS.md section 3a draws exactly this line — the loop-free
half of libuv is usable beside h2o's evloop, the loop-bound half needs a loop of its own — and a
pool would in any case have no way to express the one thing asked of the arrangement: a worker
that stays.

### Retry: once, after a pause, and then it is written down

A failed mail is tried once more after `retry_delay_ms` and then given up on, logged at error
with its recipient and its `Message-ID`. No third attempt and no growing backoff.

That is a deliberate floor rather than a first version. An unbounded retry is how a dead relay
turns into a hot loop, and a queue that keeps trying is a queue that fills up and then refuses
new mail because of old mail — the failure spreads instead of staying where it was. One retry
covers what retries are actually good for: the connection that died between two mails, the relay
that was restarting. Anything beyond that is not a transport problem and is not solved by asking
again sooner.

The `Message-ID` is assigned when the mail is rendered and survives the retry. Nothing depends on
it today, because a failed attempt is not known to have been half-delivered; it matters the
moment a retry follows an attempt that timed out *after* `DATA`, where the relay may well have
taken the mail. A stable id is what keeps that from becoming a duplicate in someone's inbox.

A first failure is neither sent nor failed in the counters. It is on the retry ring, and the
second attempt decides which it becomes.

### What the queue costs, exactly

Nothing allocates after startup. Four blocks are taken at create — the mailer, the queue ring,
the retry ring and one arena per queued message — and the host is not asked again. The ceiling is
`queue_max * message_max` plus about 600 bytes of entry per slot, and whichever runs out first
answers `SC_ERR_QUEUE_FULL`. That answer goes to the caller on purpose: only the caller knows
whether this mail may be dropped or the work behind it has to stop.

**The queue is a ring, and not a bucket vector.** A bucket vector is the right structure while a
flush empties everything at once — `clear()` keeps the buckets warm and the arena behind it
resets wholesale, so a round costs no allocation. Retry rules that out: a mail that comes back
has to outlive the round it failed in, so there is never a moment when everything is dead
together, and an append-only container consumed continuously grows without bound — which is what
the house's fixed-size rule exists to prevent. A bounded FIFO is a ring, and the shared arena is
an `arnm_fixed_arena_pool` for the same reason:
an arena frees only at its tail, and the pool hands one arena per queued message out and takes it
back in any order.

The retry ring holds `queue_max` entries, the same as the queue, so it cannot refuse — every mail
that could possibly be in flight fits. It also comes out ordered by its due time for free,
because every retry waits the same delay, which is what lets a worker decide whether anything is
due by looking at the head alone.

### One lock, and where it is not held

`lock` covers both rings, the arena pool, the counters and every worker's bookkeeping. There is
no second lock, and it is never held across `curl_easy_perform()` — a worker takes its mail,
releases the lock, sends for as long as the relay takes, and takes the lock again to book the
outcome.

Rendering is outside it too. The arena is borrowed under the lock and belongs to that thread
alone from then on, so several producers render at once and only the push itself is serialised.
Holding the queue lock across a copy of the body would put every producer behind the largest
mail.

The pool is the reason borrowing is inside: `arnm_fixed_arena_pool` says in its own header that
one pool used from two threads is a data race, and a pool per thread is not the shape here.

### What is deliberately absent

**Persistence.** A mail waiting in the queue when the process dies is gone, and so is one that
exhausted its retry — the error line is the only record it existed. This belongs in the database
and is not built yet. What it has to do when it is: a mail is written down before it is handed to
the mailer, the queue holds a reference rather than the only copy, and the outcome — sent, or
given up on — is written back. That also gives the retry somewhere to live across a restart,
which is the one thing the in-memory version cannot offer at any number of attempts. Until it
exists, anything that must not be lost has to be written down by its caller first.

**Pipelining.** RFC 2920 lets a client put `MAIL`, `RCPT` and `DATA` in one write instead of
waiting for each reply, and it is worth about a third on loopback and half against a remote
relay. libcurl will not do it — its SMTP dialogue is strictly one command per round trip — so it
would have to be written by hand, against a server whose sockets are configured for it. MailDev
advertises `PIPELINING` and never calls `setNoDelay`, which turns a pipelined batch into 44 ms
per mail of Nagle and delayed ACK. Measure before believing an advertisement.

**Several recipients per message.** See *One recipient*.


## Session cache

The C implementation holds one session store shared by all threads. This is the structure the
whole architecture rests on, so its invariants are written down rather than left to the code.

It is not a hash table. **The JWT carries the slot index, so a lookup is an array read.** There
is no key to mix, no probe walk, no collision, no full-cache case that costs anything, and expiry
is a comparison at a single known position instead of work spread over every walk.

And it is not behind the JWT verifier but in front of it. **A session holds every token it has
been issued, so a hit is a comparison rather than a signature check** — `sc_jwt_verify_hs256` is
a base64 decode, a JSON parse and an HMAC over the whole token, against a subtraction, a bounds
check, a load and two comparisons here. The store is therefore asked first, on claims nobody has
vouched for yet, and the signature is verified only when it answers nothing.

`benchmarks/bench_jwt.c` is what measures the first half of that, and its number belongs in this
file once someone has run it. The reference path's numbers are already in `../Architecture.md`,
*Session cache*, and the ordering they establish carries over: the signature is the **smaller**
half of what a hit saves. The larger half is the session a hit does not have to build, which
here is *Where a query's time goes* — 37 to 48 µs before anything is answered.

```text
one bucket_vector of session pointers, indexed directly by the token, and it grows
one queue            the slots in creation order; expiry walks it from the front
one free list        slots whose session has ended, ready to be handed out again
one token set        per session; what a hit is decided by, instead of the signature
reference counting   unchanged, and it is what makes releasing a slot safe
```

The store is not told how large it should be. How many sessions live at once is the number
created within one `SESSION_HARD_TIMEOUT_MS`, which depends on the community and the hour, so
the store grows a slot at a time and reuses the slot of every session that ends. The
configured maximum is a crash guard — see *The queue and the ceiling*.

### The token carries the slot, and the session carries the token

A hard timeout on a token that is never refreshed means sessions expire in creation order. The
JWT carries `session_created_at` for that, the index of the slot its session was placed in, and
the `user_uuid` it belongs to. A re-issued token copies all three unchanged; the policy around
that — one new token per minute, a login of thirty — is in `../Architecture.md`, *Tokens and the
login*, and not here.

```text
read the claims, verifying nothing: session_created_at, user_uuid, slot
require every one of them                       <- absent is not the same as zero
check now - session_created_at < SESSION_HARD_TIMEOUT_MS    else -> verify
range-check the slot against the slots that exist          else -> verify

under the store lock, shared:
    read the slot; empty => verify
    check the ENTRY's own session_created_at against the timeout   else -> verify
    check the entry's user_uuid against the claim                  else -> verify
    take a reference
release the store lock

under the session's lock, shared:
    is the token one of the tokens this session was issued?
        no  => release the reference, verify
        yes => a hit, and nothing had to be verified to reach it

verify: the signature, then the claims, then exp -- and then create a session
```

**Nothing the token says is trusted as data.** The claims address a candidate; every decision is
made against what the store itself holds. A token is accepted because it is byte-identical to one
this process minted and still holds, which is exactly what the signature would have proven,
established by equality instead of by arithmetic. That is also why the token comparison is last:
everything before it narrows the search and none of it may decide anything on its own.

The claim's age is checked first because it is free and keeps a long-dead token off the store
entirely. It is **not** what ends a session — the entry's own `session_created_at` is, because
that is the one this process wrote. The two differ in exactly one case, a session whose timeout
has passed while the expiry walk has not yet reached its slot, and under the previous design, where
the signature was verified before anything was read, one check did both jobs. It does not any
more, and that is the single most important consequence of looking before verifying.

A client that comes back after eleven minutes costs one verification and one insertion; a client
inside the window costs a bounds check, a load and two comparisons, and never touches libsodium.

**A missing claim must be a miss, because the C default for a missing integer is zero and zero is
a valid slot.** *Safety net* says a claim that is absent is not a claim that passed; on this path
that rule covers every claim there is, since none of them has been vouched for.

TypeScript declares those rules as a schema — `sessionClaimsSchema` in
`packages/service-core/src/session/input.schema.ts` — and this path has to reject exactly what it
rejects: an absent claim, a slot that is not a whole non-negative number, a `user_uuid` that is
not a uuid. The arnm reader makes that easy to get wrong in one particular way, recorded in
`AGENTS.md` section 6: it keeps its first error and every getter after it answers empty, so a
claim of the wrong type silences the reads that follow, in another field entirely. Ask
`arnm_json_reader_type_of()` or `_has()` where a claim may legitimately be absent. Two
implementations disagreeing about which payloads are nonsense is a bug neither of them reports,
which makes this the natural first entry in `contracts/test-vectors`.

**The `user_uuid` check is not a formality, it covers the one case where a slot can be reused
under a live token**: a slot is reused as soon as its session is gone, and at the ceiling the
oldest live one is retired to make room — see *The queue and the ceiling*. The token comparison would catch that too. The uuid comes first because 36 bytes are
cheaper to reject than a walk over a token set, and because the entry's identity is readable
under the store lock while the token set is not.

**Where the two locks fall is decided by what changes.** `user_uuid` and `session_created_at`
are written once when the entry is created and never again, so they are read under the store
lock. The token set grows whenever a token is re-issued, so it is the session's own data and is
read under the session's lock and written under it exclusively — *Two lock layers* forbids
holding the store lock across that, and nothing here needs to. Re-issuing a token touches the
session and never the store.

**The token set is what a session pays for this**, and it is bounded by the re-issue interval:
one token per `JWT_TOKEN_REISSUE_AFTER_MS` for as long as a session lives, so eleven at most.
Whether a token is due is decided from the server's clock and never from the token's `iat` —
believing an unverified `iat` would hand the size of that set to whoever writes the token. `exp`
is not checked on the hit path and does not need to be: a token in a live session's set was
issued no earlier than that session began, the session is younger than ten minutes, and thirty
is longer than ten.

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

The first of that work is the read path's own last step: the token comparison reads the session's
token set, which grows when a token is re-issued and is therefore the session's data, not the
store's.

Incrementing after the release is a use-after-free. In the gap another thread releases the
session, the count falls to zero, the memory goes back to the arena, and the increment then
lands in it. Holding the shared lock keeps the expiry walk out, because releasing a slot needs
the exclusive one — and a session still in the store has a count of at least one, so it cannot
be freed while it is being read.

Direct indexing makes this lock cheap enough to keep and small enough to remove later. It is held
for two instructions, and *Open* records what it would take to drop it entirely.

### The vector

```text
bucket_vector of pointers, chunks of a fixed size, chunks never move
size            what the load turned out to be; it grows, and it does not shrink
8 bytes a slot  one arena for the sessions themselves
free list       intrusive: an empty slot holds the index of the next free one,
                so reuse costs no memory of its own
```

A bucket vector rather than a flat one, for a reason that is about concurrency and not about
allocation, and it is the same reason growth is possible at all here: growing a flat vector
moves every element, a reader holding a slot address across that move reads freed memory — and
worse, every slot number already out in a token would name something else. Chunks that never
move make growth a published pointer to a new chunk and nothing else. Readers in existing chunks
are untouched, a reader whose index is beyond the current size takes a miss, which is already a
case the read path handles, and **a live session is never moved, because its index is out in the
world inside tokens this process signed.**

There is no hash here, and that removes a whole failure class rather than a line of code. *What
was measured* records two reproductions where a colliding key set does not make a bounded-probe
cache slow but makes it stop holding anything — correct answers, hit rate on the floor, nothing
in the log. An index that the server itself handed out cannot collide, cannot be strided, and
cannot be chosen by an attacker. The seeded splitmix mix and `CACHE_PROBE` stop being session
concerns; they remain the rule for any cache that still derives a slot from a key.

### The queue and the ceiling

One queue of slot indices in creation order, and its front is the only moving part of expiry.
Every session dies exactly `SESSION_HARD_TIMEOUT_MS` after its creation and they are pushed in
the order they were created, so the oldest live one is always at the front. Expiry therefore
needs no sweeper thread, no walk of the whole store and no check on the read path. It needs one
comparison, at one known position, paid by whoever creates a session.

Creating a session:

```text
at the front of the queue, while the entry there has timed out or is already empty:
    drop the store's reference  <- the session is freed if the count reaches zero
    clear the slot, push it onto the free list, advance the front
take a slot: the free list if it has one, otherwise append one to the vector
write the session, push the slot onto the back of the queue
```

The walk keeps going for as long as it finds timed-out entries, so a burst of expiries goes back
to the arena at once rather than one insertion at a time; it stops on the oldest entry that is
still live, and nothing else moves the front. In the steady state the slots it frees are exactly
the ones the next insertions take, and the store neither grows nor shrinks.

**The queue cannot be read off the vector.** A slot is reused as soon as its session is gone, so
slot order stops being creation order the first time that happens — and creation order is the
one premise expiry has. Four bytes an entry, and the free list is intrusive, so the pair costs
one `uint32` a slot and no allocation.

**The store is not sized in advance, it grows into its load.** How many sessions live at once is
the number created within one timeout window; nobody knows it in advance, it differs per
community and per hour, and it is exactly what appending a slot when none is free discovers.
What the vector settles at is the answer, and it stays there because slots come back.

**The ceiling is a crash guard, not a size.** Below it nothing is ever retired early. At it, and
only there, the oldest live session is retired to make room: the store drops its reference, a
session a request is still working on survives, owned by that request until it finishes, and its
owner's next request costs one verification and a fresh session. This is the only path that can
retire a session before its timeout, it is the reason the `user_uuid` check exists on the read
path, and it degrades as a cache miss rather than as an error. **Insertion never fails.**

**What that number should be is open, and it is a memory question.** *The working set needs a
number* has what a session costs; what it does not have is a measurement of the whole process
under a load that fills the store. That is the experiment: resident memory against the number
of live sessions and the number of slots ever needed, both of which the store already knows.
Bytes per session is the slope, the ceiling is the memory the machine can spare divided by it
with room left over, and it belongs in configuration rather than in `contracts/const.json` —
the two implementations do not spend the same memory on the same session and must not share a
number that pretends they do.

The walk is short, it is bounded by the number of entries that expired since the last insertion,
and it touches nothing but slots, the queue, the free list and reference counts — so it runs
under the store's exclusive lock without holding it across anything that can block. Freeing a
session whose count reaches zero returns arena memory and takes no other lock; *Two lock layers*
is why that is allowed to happen there at all.

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
the expiry walk     clears the slot and gives up that reference
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

What one session costs, by how much of a ledger it is allowed to hold:

| transactions per session | session |
|---|---|
| 25 (one page) | 8,0 KiB |
| 50 | 15,0 KiB |
| 200 | 57,2 KiB |
| 500 | 141,6 KiB |

Unbounded, the ledger is the entire footprint: at 500 transactions the identity data and
every mutex in the session together are under half a percent of it. The bound is therefore
not a tuning parameter, it is the design.

Keep roughly two pages — `DEFAULT_PAGINATION_PAGE_SIZE` is 25, so about 50 — extend forward
through the pagination cursor, and read older windows from the database when someone actually
pages back. That is 15 KiB a session against 142 — nine times as many of them in the same
memory, for a ledger nobody asked to see.

The other lever is in `contracts/db/transactions.json`, recorded there as open: the two
denormalised `varchar(512)` names and four uuids per row are 116 of the 288 bytes. Holding
ids in the session and resolving names where they are rendered gives back another 40%.

The tokens do appear in it. A session holds up to eleven of them — *The token carries the slot,
and the session carries the token* has the arithmetic — at a few hundred bytes each, so a few
kilobytes on top of every row above. At 25 transactions that is roughly a third of the session,
which is the honest price of not verifying a signature on every request, and it is charged only
to sessions that live long enough to be refreshed ten times.

Two ways to bring it down, both open. Keep only the newest two tokens rather than all of them:
the client uses the newest it was given, and the older ones only matter for requests that were
already in flight when it was refreshed — at the cost that a client which keeps presenting an
older one gets a new session on every request instead of a hit, which is correct and slow.
Or hold a digest instead of the token, which is cheap in memory and needs an argument the token
comparison does not: a digest small enough to be worth it is a keyed hash whose forgery
resistance has to be justified, and a cryptographic one costs about what the HMAC being avoided
costs.

The index itself barely appears in this arithmetic and that is the point of it. A thousand
sessions at twelve bytes a slot — the pointer, plus four for the queue entry — are 12 KiB, a
tenth of a percent of what those sessions cost. Growing therefore costs nothing worth counting,
and the ceiling can be read off the sessions alone: there is nothing to weigh an index
against.

Which makes this table the closest thing there is to a ceiling until the experiment in *The
queue and the ceiling* has been run — and a C number: the TypeScript path spends several times
as much on the same session, which is exactly why the ceiling is configuration rather than a
shared constant.

### What was measured, and what it changes

`../../h20Test/bench_cache` measures hash-based caches: an open-addressed table with a hard
timeout, a `uint16_t` slot index, and several ways to handle a full one. `rand` keys, one
Ryzen 7 5700G, single threaded, nanoseconds per lookup. Three of its results are why the session
store has no hash at all, and they are the defence against re-deriving one here later. The rest
holds for any cache in this project that does still derive a slot from a key.

**Lazy expiry during the probe walk is not a shortcut, it is the reason to use open
addressing at all.** A map-based cache cannot reclaim in passing and has to sweep; at a
million live entries that is 1 730 ns per operation against 215 for the open-addressed one,
and the whole difference is the sweep. Behind a shared lock that is the difference between a
cache and a stall. The queue is the end of that line of argument: reclaim happens at one known
position, with no walk to ride on.

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
super-linear, where this one does not overfill — it grows to the load, and only at the ceiling
does it cost a session retired early, priced in *The queue and the ceiling*.

**The hash is the entire safety margin, and `CACHE_PROBE` is what makes that true.** A
bounded probe means a colliding key set does not make the cache slow, it makes the cache stop
holding anything — correct answers throughout, hit rate on the floor, nothing in the log. Two
such failures were reproduced: a masked table over an unmixed key dies on keys that stride by
the table size, and `std::unordered_map` under libc++ *is* that masked table, because
`reserve(65536)` yields a power-of-two bucket count indexed by masking. 19 ns became 25 µs.

Mix the key — splitmix64 finalizer, two multiplies — **and seed the mix once per process**.
The seed is what makes an adversarial key set unconstructible; it costs one XOR. **That rule
binds every cache here that derives a slot from a key.** The session store is not one of them,
and this result is the strongest single reason why: the failure is silent, is reachable from
outside, and cannot occur at all once the server hands out the slot instead of computing it.

The slot holds a pointer and there is no table count to size against the memory: the store grows
to whatever the load turns out to be, and the only number left to decide is the ceiling, which
*The working set needs a number* is the beginning of.

### What the store guarantees, and what it needs from the token

`../../h20Test/bench_session_cache` measured **30 bulk reclaims where a hash-routed equivalent
did 386 260 inline ones**, and 8 to 21 % less time per request between sixteen thousand and a
quarter million live sessions. Expiry is off the request path and out of the background: no
sweeper, no per-minute pass, one comparison at the front of the queue.

A refreshed token copies `session_created_at` and the slot, so it addresses the session it
already had and the store never learns that anything happened. The only thing that creates a
session is a genuine miss, which is what keeps creation order — the order the queue depends on —
maintained by construction rather than by anything the refresh path has to remember.

Dropping a slot is dropping the store's own reference to what is in it: the slot is cleared, the
count falls by one, and a session a request is still working on survives, owned by that request
until it finishes. Clearing the index never touches the working set either — the index holds a
pointer, the working set lives inside the session.

**Check `now - session_created_at >= SESSION_HARD_TIMEOUT_MS` against the token before the store
is touched at all.** What that check is worth changed when the store started being asked before
the signature is verified:

```text
what it proved while the signature was checked first
    check now - session_created_at >= SESSION_HARD_TIMEOUT_MS against the token, first
    => no valid token addresses a slot expiry is releasing
    => no reader can still ENTER such a slot; only readers already inside it
    => what protects the read path is a grace period, not a lock

what it proves now that the store is asked before the signature is verified
    => a forged claim can name any slot at any time, for free
    => the check is a filter that keeps dead tokens off the store, nothing more
    => what protects the read path is the store lock, and the entry's own
       session_created_at is what ends a session
```

**That is the price of looking before verifying, and it is worth paying.** The lock was going to
be taken anyway — *Open* below already argued for it and the walk it protects runs once per
session creation — while the saving is an HMAC on every request that hits. What is gone is the
option of removing the lock in favour of a grace period; the epoch counter remains, because it
does not depend on anything a token says.

**A claim from the future costs nothing to write, so nothing may rest on it.** An issuer whose
clock runs ahead produces one honestly, and anyone at all produces one by typing it: on the read
path the claim carries no signature. `session_created_at <= now + skew` therefore stays where it
can mean something, in JWT validation on the verify path, and the read path defends itself
instead — the entry's own creation time, the `user_uuid` and the token comparison are each
against something this process wrote. The failure that leaves is contained to one lookup, one
mismatch and one verification.

### What it is worth at the size this targets

The `bench_session_cache` ladder starts at a thousand live sessions, and one community's live
sessions can sit anywhere below the bottom of it; since the store grows to its load there is no
configured size to hold up against it either. A bounds check, a load and an atomic increment have
no size at which they lose, but the honest version is that at the sizes one community reaches,
none of this is what makes a request slow. The reasons to take this design are the ones that do
not appear in a single-threaded nanosecond column at all:

- expiry leaves the request path entirely, and leaves the background too — there is no sweeper
  and no per-minute pass, only a comparison paid by whoever creates a session
- there is no hash, so the silent hit-rate collapse in *What was measured* is unreachable
- the store lock is held for a handful of instructions, which the probe walk never was. It is
  no longer a candidate for removal — asking the store before verifying is what took the grace
  period off the table, see *Open* — but an epoch counter still is
- the index costs 8 bytes a session and the queue four more, so growing into the load is cheap
  and the ceiling is a safety bound rather than a size anybody has to guess
- a request that hits verifies no signature, which is the largest single item this design saves
  and the only one that grows with the number of requests rather than with the number of sessions

### Open

**Protecting the read path against the expiry walk.** A reader must not be inside a slot while
the walk frees it. Two ways to ensure that, and the third that the previous design had is gone:

- the store rwlock, shared on lookup and exclusive for the expiry walk. Trivially correct, and
  cheap here: the walk runs once per session creation, not on every miss. Its cost is the reader
  counter's cache line, which *What was measured* puts above eight
  cores — and this machine has four.
- an epoch counter the reclaimer waits out. Costs nothing on the read path and is correct, and it
  is real concurrent code with real ways to get it wrong.
- ~~release with a margin~~, letting the walk treat an entry as gone only at `session_created_at
  + timeout + grace`. This rested on the token's TTL check keeping every reader out of a slot
  about to be released, and that argument died with the signature check moving behind the lookup:
  a forged claim reaches any slot at any time. It was never a proof anyway — a descheduled thread
  outlasts any margin — but now it is not even an approximation.

Take the lock now, and let a profiler rather than this section decide whether the epoch counter is
ever worth its risk.

**Session working sets grow lazily while only a shared lock is held.** Allocating from the
session's arena at that moment races with the other readers, and this is unchanged by anything
above — the store's structure was never what made it hard. The options differ enough to matter:

- a per-session sub-arena, reserved with a cap when the session is created
- growth upgrades to the write lock
- per-session chunks with their own synchronisation

Until it is decided, nothing on the read path may allocate.
