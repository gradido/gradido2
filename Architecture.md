# Gradido2 Architecture

## Scope

Gradido2 is a **rebuild of gradido legacy on a new stack**. No legacy code is carried over,
but the complete feature set of gradido legacy is the target — this is a replacement, not a
subset and not a second product.

`https://github.com/gradido/gradido` remains the behavioral reference for every feature being rebuilt. Where the
intended behavior of a rebuilt feature is unclear, the legacy implementation decides, and
the answer is then written down in `contracts/test-vectors` so it is decided only once.

## Core principle

Gradido2 uses a **reconstructible, session-local working context**.

The database remains the source of truth. The session is a materialized, ephemeral view of the data the current user has already needed.

> RAM may forget. The database must not.

A server restart, session loss, or request being routed to another instance must never make the application incorrect. The missing in-memory state is rebuilt lazily from the persistent source.

The primary optimization is therefore not making database access slightly faster, but **avoiding repeated database work altogether**.

## Why two implementations

Gradido2 exists twice:

- **TypeScript** (bun, ElysiaJS, valibot), as workspaces in `packages/` — the reference implementation. It defines the business behavior.
- **C** (with C++ in leaf modules), in `fast-servers/` — a fast implementation that lazily mirrors the TypeScript side. It is allowed to lag behind.

There are two reasons, and they are different in kind.

### Continuity

The TypeScript path is the answer to "who maintains this if the author is unavailable".
TypeScript developers are easy to find; developers who will maintain an arena allocator and
a sharded session map are not. If that situation ever occurs, the TypeScript path is fixed
and developed further, and the fast path either follows through AI-assisted translation or
freezes without taking the product with it.

Two rules follow, and they are not optional:

- **No feature originates in the fast path.** Behavior that exists only in C silently
  removes itself from the fallback.
- **The fast path must be droppable, not merely removable.** Running without it must
  require no code change: no shared state, no route that exists only there, no role only it
  fills.

The consequence is that the TypeScript path must stay *independently shippable*, including
its own single-binary release. A fallback that cannot produce a release is a specification,
not an insurance policy.

### Density

Measured in a test project, same pipeline (JWT → PostgreSQL → JSON) on both stacks:

| | CPU per request | RSS |
|---|---|---|
| C on h2o, cached | 11,6 µs | 15 MB |
| Bun + Elysia, tuned, cached | 23,2 µs | 102–125 MB |

The RAM figure matters more than the CPU figure. A Gradido community server should be
hostable by people who are not server administrators, on the smallest hardware that will do
— and there the difference decides whether it runs beside its database at all.

There is a second, structural reason: Bun scales across cores by `SO_REUSEPORT` processes,
each with its own SessionContext map. Eight workers mean eight working sets for the same
users, or sticky sessions — which this architecture rejects. A multithreaded fast server has
one session map across all cores. The session model of this document therefore works fully
only on the fast path.

## Three kinds of code

Not everything is mirrored. Before writing anything, decide which of these it is.

| Kind | Lives | Mirrored |
|---|---|---|
| **Determinism-critical** | once, in C, in `shared-native` | **never** |
| **Domain / business** | TypeScript reference | yes, C follows |
| **Infrastructure** | each implementation its own | no |

`shared-native` is not a performance device. Its purpose is that a computation produces the
same result everywhere. `../gradido/shared-native/src/data/unit.c` carries the evidence: the
decay factor derived from Decimal.js and the fixed-point one differ in the last unit, and
the TypeScript value is commented out. Two implementations of money arithmetic do not
produce different speeds, they produce different amounts.

TypeScript calls into it through N-API with `bigint` at the boundary; the fast server links
the same C directly and pays nothing.

Every piece of logic moved into `shared-native` is a piece that needs no mirror and cannot
diverge. 

`contracts/` covers what remains: constants, schemas, test vectors and API interfaces, in
JSON, tested against both implementations. Automated tests make the gap visible — a failing
or skipped test on the fast path documents a feature that is not implemented there yet.

## Amounts

Amounts are `bigint` in gdd units, never `number`, on both sides.

```text
add, subtract, multiply   exact in both languages, may be written inline
divide, round, decay,     always through shared-native, in TypeScript and in C alike
parse, format             never reimplemented in either
```

The functions exist: `calculateDecay`, `toDecimalPlaces`, `gradidoUnitFromString`,
`gradidoUnitToString`, `getDecayStartTime`, `getDecayRespiteCent`.

## Portability of the reference implementation

The TypeScript side is written so that translating it stays tractable. These cost nothing
and make the difference between a mechanical port and a rewrite:

- domain data as flat, serializable structures — no class hierarchies, no state captured in
  closures
- IDs instead of object references inside the SessionContext; pointer graphs do not port
- no business outcome that depends on JavaScript semantics: Map iteration order,
  `undefined` versus `null`, implicit coercion
- amounts as above

## Repository layout

```text
packages/          TypeScript — reference implementation
  backend          runnable HTTP server (routes, wiring, startup)
  backend-core     backend domain code: data, logic, interactions, repositories
  federation       federation server
  admin            admin frontend
  frontend         user frontend
  frontend-core    UI code shared by admin and frontend
  shared           code shared by frontend and backend, e.g. route definitions
                   (so Eden Treaty can derive types) and shared valibot schemas
  shared-native    determinism-critical C, called from TypeScript via N-API
                   and linked directly by the fast servers

fast-servers/      C — fast implementation, mirrors the domain structure
  backend
  backend-core
  federation
  dht-node

contracts/         language-independent JSON contracts, see below
```

The `-core` packages contain the domain implementation; the packages next to them
are the deployable applications that wire it up. Business code belongs in `-core`.

Empty directories are intentional. They describe where code belongs once it exists.

## Contracts

`contracts/` is the shared, language-independent description of behavior that both
implementations must satisfy.

```text
contracts/const.json      constants valid for both implementations
contracts/types           shared type/schema definitions
contracts/db              table and column definitions
contracts/server          route definitions: path, method, request, response
contracts/errors          error codes and their meaning
contracts/test-vectors    input/expected-output pairs for both implementations
```

When behavior that both implementations share changes, the contract changes with it.
The contract is a documentation of the TypeScript code, but more important, it is the agreement the
fast path is tested against.

## Testing

- TypeScript: `bun test`
- C: google test
- Contract tests read `contracts/` and run the same vectors against both implementations
- Database tests run against both PostgreSQL and SQLite

A missing feature on the fast path should surface as a failing or explicitly skipped contract
test, not as silence.

## Session as working context

After the AppContext, the SessionContext is the main object passed as a parameter
into business functions.

It holds the working set currently useful to one user, for example:

- jwt keys for fast verification (worth measuring)
- the authenticated user with contact data, role(s) and permissions
- transactions, contributions, transaction_links, contribution_links, contribution_messages that have already been loaded
- the last known id of those tables at the time they were selected, so it can be compared against the id in the AppContext and refreshed when that data is requested again
- other data the current interaction/session needs repeatedly

Data is loaded **lazily**, when needed.

The session should have a bounded working set so that one unusual request cannot turn it into an accidental copy of a large part of the database.

The session stores its creation time and is dropped after 10 minutes (const). This is a
backstop against cache-invalidation bugs: even a session that missed an update cannot
stay wrong for long.

## AppContext

- contain db connection
- logger
- Global Caches
  - communities
  - config
  - last 500 (const) contributions (public data set) (for display contribution infos from other)
- last known id of transaction and similar tables
- Map with sessionContexts by user id
- APIs (server connections to external services)
- basically everything that was a singleton in gradido legacy

### Multiple instances

```text
             Load Balancer
              /          \
             v            v
        Instance A    Instance B
        Session A     Session B
             \          /
              \        /
                 Database
```

A user may reach another instance and therefore encounter a cold session. That is acceptable.

Sticky sessions, shared session state, Redis, or distributed cache infrastructure are not required for correctness. They may be introduced later as performance optimizations if justified.

## Logging

- Pino in TypeScript, spdlog on the fast path
- structured, machine-readable logging with a message field for human readability
- format: JSON, both implementations emit the same JSON output
- as far as it makes sense, both implementations log the same events with the same structure and data; TypeScript is the reference

## DB

Table and column names use snake_case, plural for table names, singular for column names.

- PostgreSQL/SQLite with DrizzleORM and the native bun sql driver on the TypeScript side
- PostgreSQL/SQLite with native C drivers on the fast path (fast-servers)
- prepared statements for standard queries, and where possible also for more complex, rarely used queries
- which database is used is decided at startup via config
- use the full feature set of PostgreSQL; mirror features SQLite lacks with combinations of simpler queries, and if that is not enough, process the data in TypeScript or C directly
- PostgreSQL is the reference and the default for server mode, SQLite is for easy deployment of small setups
- the server admin decides on first run which one to use; there is currently no migration between SQLite and PostgreSQL data sets
- tests run against both database modes

## HTTP server

- ElysiaJS + Eden Treaty on the TypeScript side. Route definitions belong in `packages/shared` so that frontend-core, frontend and admin can import the types.
- h2o on the fast path, configured to not allocate/free memory during operation: it starts with enough memory and reuses it.
- Routes are additionally described in `contracts/server` as JSON, so both implementations can be tested and compared.

## Config

- env for variables needed at startup (db, ports, etc.)
- secrets in production via OS-native secret stores (e.g. systemd credentials on Linux)
- secrets in dev via env
- fixed settings as constants in code, dynamic settings in a settings table, editable from the admin frontend; admin only, no separate rights are created for this

## Setup

- bun + turborepo + tsgo + biome on the TypeScript side
- zig as C/C++ compiler and as package manager for compatible third-party libs
- zig as compiler for the shared-native module used from TypeScript
- clang-format for linting C/C++ code
- google test for testing C/C++ code

### Language roles

```text
C      the fast server: h2o, request path, session, repositories.
       Also shared-native. This is where most native code lives.
C++    leaf modules only, behind an extern "C" header:
       Justified by a library without a C equivalent
zig    build system and cross compilation. No application code —
       its API still moves between versions.
```

### The self-provisioning build

`shared-native/build.ts` downloads a pinned Zig version for the current platform and builds
the native module. A TypeScript developer runs `bun install` followed by
`turbo backend#start` and needs to know nothing about the toolchain.

This is part of the continuity plan, not a convenience: the TypeScript fallback path is not
C-free, so it stays viable exactly as long as it keeps building itself. Two properties
therefore matter beyond ordinary tooling hygiene — the downloaded archive should be verified
against a pinned checksum, and the documentation should state how to build if the download
URL is ever unavailable (place any Zig of the pinned version in the expected directory).

The pinned version belongs in AGENTS.md as well, so that agents stop guessing it.

### Safety net for native code

Non-negotiable wherever C runs, and more so where it was AI-generated:

- ASan, UBSan and TSan in CI, not only locally. TSan matters most — the shared session map
  is the one defect class expert review does not catch.
- Fuzzing for every parser that touches attacker-supplied bytes: JWT and JSON. The signature
  is verified before anything else is read; everything after it is hostile input.
- Contract vectors as a merge gate, green on both implementations.

## Business logic around the session

The session should be part of the **application/business context**, rather than a generic cache service hidden underneath the business logic.

The goal is that a developer can read an interaction and immediately see:

- which data it uses,
- which data it changes,
- which session state it updates,
- how freshness/invalidity is handled.

over a generic global cache layer that hides invalidation behavior.

Cache invalidation is part of the business semantics of an operation and should therefore live close to the logic that understands those semantics.

### Auth - Roles and Rights

- Rights are defined in code as enums with a string -> number mapping, one enum per domain, optimized for bit operations
- Default role rights live in code: admin is allowed everything; user, moderator and ai-moderator each have an explicit default set
- Roles in the database can inherit from the default roles to extend or restrict them. The default roles themselves cannot be overwritten.
- Rights are stored as strings in the database and used as a bitset at runtime. Unknown strings from the database are ignored and logged as a warning.
- Max 64 rights per domain, so a domain's rights fit into the bits of a uint64
- Global cache for role rights, max TTL 10 minutes, invalidated when an admin edits a role
- Routes that need no permission (login, viewing community info, ...) are grouped in one file
- Every non-public request with a JWT creates a new token, so an active user's session timeout keeps moving. The session context is still dropped after 10 minutes (hard timeout).

Tables:

```text
roles:      id, parent_role (varchar, optional), role (varchar), description (text)
role_rights: id, role_id, domain, right (varchar), created_at
```

`parent_role` is a string rather than a foreign key because the default roles are
defined in code and have no database row. `role_rights` stores one right per row.

## DCI: Data, Context, Interaction

DCI is used as a business-logic organization principle.

### Data

Represents what exists:

- User
- Transaction
- Community
- etc.

Data should contain the state and simple operations that naturally belong to that data.

### Data-Logic

Logic that operates directly on data and is too small/simple to justify a separate interaction.

Examples:

- calculate decay
- calculate balance
- validate a transaction
- determine whether a value is expired

A useful rule:

> If the operation is essentially “given these data, calculate or validate X” and does not orchestrate a larger business action, it is Data-Logic.

Do not create an Interaction merely to give every function a formal wrapper.

### Interaction

An Interaction represents a business operation involving context, multiple pieces of data, persistence, side effects, or session state.

Examples:

- create transaction
- cancel transaction
- add community member
- rename community
- load transaction history

The Interaction is the readable “story” of the business operation.

## Source organization

Organize primarily by **business domain**, not by technical layer or database table.
The top-level domains should follow business capabilities rather than blindly mirroring database tables.

The same domain structure exists in TypeScript and C, so that a file on one side
points at its counterpart on the other:

```text
packages/backend-core/src/domain/community/interactions/add-member.ts
fast-servers/backend-core/domain/community/interactions/add-member.cpp
```

This is a navigation aid, not a requirement that both files be structured the same way
internally.

Within a domain, the DCI roles are distinguished by file suffix:

```text
*.data.ts          domain state
*.logic.ts         small logic operating directly on data
interactions/      one file per business operation
```

## Session implementation boundary

The generic session mechanism belongs near the application layer, but the semantics of
cached domain data belong to their domain.
Avoid a giant generic session/cache module containing all domain-specific invalidation rules.

## Consistency model

Do not use one cache policy for every kind of data.
Classify data according to how it changes and what freshness it requires.

Typical categories:

### User-owned data

If only the current user can modify it, it can often remain in the session for a long time.
When the application itself changes it, update the session immediately.

### Append-only data

Transactions are a particularly useful example.

Instead of asking whether the entire cached transaction set is still valid, keep a monotonic sequence/generation/cursor:

```text
Session:
    transaction_sequence = 4711

Current:
    transaction_sequence = 4717
```

The session can then load only the missing range.

```text
4711 -> 4712, 4713, 4714, 4715, 4716, 4717
```

This turns cache invalidation into incremental synchronization.

### Data modified by other users

Do not attempt to find every session that might contain the data.

Prefer a version/generation on the data.
This avoids global invalidation tracking.

### Volatile data

Use stricter validation, shorter lifetimes, or avoid caching it when stale data is unacceptable.

## Own writes vs. foreign writes

Use an intentionally asymmetric strategy.

**Own writes** — the current Interaction performs the change. It knows exactly what
changed, so it updates the session directly if easy (no extra logic envolved) or invalidate the cache part if not needed in the current request

**Foreign writes** — someone else changed the data. Do not try to find every session
that might hold it. Let the data carry a version/generation/cursor, and let each
session notice on access that its copy is behind:

```text
version/generation changes
        -> session detects stale state when the data is next used
        -> session refreshes
```

The asymmetry is deliberate: precise updates where the knowledge exists, lazy
detection where it does not. Neither direction requires distributed bookkeeping.

## Repository boundary

Business interactions should not contain raw database access details.

Prefer:

```text
Interaction
    |
    v
Repository
    |
    v
Database
```

The repository is the persistence boundary.

The Interaction decides when data is needed and when session state must be updated. The repository knows how to retrieve or persist it.

This keeps the consistency strategy visible in business code without coupling the business logic directly to PostgreSQL/SQLite or Drizzle details.

The important architectural property is that the session update is visible directly beside the business operation that caused it.

## Session state is not automatically globally consistent

A session is allowed to be stale according to the policy of its data.

The design should distinguish:

- **absent** — data has not been loaded
- **loaded/current** — data can be used
- **stale** — data exists but must be refreshed before use

Stale data does not necessarily need to be discarded. It can often be refreshed in place.

## Session vs. global cache

These are different concepts.

### Session working context

Answers:

> What has this user/session already loaded and is likely to need again?

### Global cache

Answers:

> Has any request/instance recently loaded this generally useful object?

A system may use both, but neither should become the source of truth.

```text
                    Database
                       ^
                       |
             +---------+---------+
             |                   |
       Global cache           Session
             |                   |
       shared hot data       user working set
```

## Design principles

1. **Avoid work before optimizing the work.**
2. Prefer eliminating repeated database calls over micro-optimizing individual calls.
3. Treat RAM state as disposable.
4. Keep the database as the source of truth.
5. Load session data lazily.
6. Let data behavior determine its cache/freshness strategy.
7. Put invalidation and refresh rules close to the business logic that understands them.
8. Update the current session directly when the current interaction performs the write.
9. Detect foreign changes through versions/generations/cursors where possible instead of tracking every affected session.
10. Exploit natural properties of the data:
   - append-only data -> sequence/cursor
   - versioned data -> version check
   - user-owned data -> long-lived local state
   - volatile data -> strict freshness policy
11. Do not introduce generic infrastructure merely for symmetry.
12. Keep the hot path simple and make the data flow obvious from the code.

## Architectural goal

The intended runtime behavior is:

```text
Cold request:

HTTP
  -> Session
  -> Repository
  -> Database
  -> Session populated
  -> Business logic
  -> Response

Warm request:

HTTP
  -> Session
  -> Business logic
  -> Response
```

The database is therefore used primarily when information is **actually absent or stale**, rather than as a mandatory dependency of every request.

The architecture deliberately accepts redundant, disposable in-memory state across server instances in exchange for:

- fewer database round trips,
- simpler horizontal scaling,
- no requirement for sticky sessions,
- no distributed cache dependency for correctness,
- and business-visible consistency rules.

The central architectural principle is:

> **Keep the hot working context close to the business logic, make its consistency rules explicit, and make every in-memory optimization safely disposable.**
