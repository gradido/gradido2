# Gradido2 Architecture

## Core principle

Gradido2 uses a **reconstructible, session-local working context**.

The database remains the source of truth. The session is a materialized, ephemeral view of the data the current user has already needed.

> RAM may forget. The database must not.

A server restart, session loss, or request being routed to another instance must never make the application incorrect. The missing in-memory state is rebuilt lazily from the persistent source.

The primary optimization is therefore not making database access slightly faster, but **avoiding repeated database work altogether**.

Gradido2 uses a TypeScript Implementation (bun, elysiaJs, valibot) as reference as workspaces in package, and a fast implementation in C++ in fast-servers,
which lazy mirror the whole TypeScript Implementation. It didn't need to be up to date all the time. It is prefered to have automatic tests which show which features currently missing in C++-Implementation.
Contracts contains json files describing consts, schemas, test-vectors and api-interfaces which can be tested against both implementations, to make sure they behave the same. 

## Session as working context

A SessionContext is after the AppContext the main object used as parameter in many functions

It contains the user's currently useful working set, for example:
- jwt keys for fast verification (measure)
- authenticated user with contact data, role(s) and permissions
- transactions, contributions, transaction_links, contribution_links, contribution_messages already loaded 
- last known id of this tables on last select, so it can be compared with id in appContext and refreshed if data from this tables are requested
- other data repeatedly needed by the current interaction/session

Data is loaded **lazily**, when needed.

The session should have a bounded working set so that one unusual request cannot turn it into an accidental copy of a large part of the database.

The Whole Session store it's creation date and will be auto-deleted after (const) 10 Minutes. This is for fix possible cache-invalidate errors

## AppContext

- contain db connection
- logger
- Global Caches
  - communities
  - config
  - last 500 (const) contributions (public data set) (for display contribution infos from other)
- last known id of transaction and similar tables
- Map with sessionContexts by user id
- Apis (Server Connection to extern services)
- basiclly everything what was singleton in gradido legacy
- 

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

- Pino in TypeScript, spdlog in c++
- structured, ki-friendly logging with message for human readablity
- format: json output, both use same json output
- as long as it make sense, both path log the same events with the same structure and data, TypeScript is reference implementation

## DB

Table and column name in db are using_undercores, plural for table name, singular for column names
- postgresql/sqlite with DrizzleORM and native bun sql driver on TS side
- postgresql/sqlite with native C/C++ postgresql/sqlite driver on C++-Side (fast-Servers)
- prepared statement for standard queries, if possible even for more complex, not much used queries
- Decide on runtime start via config which db is to use
- use full features of postgresql, mirror not existing features on sqlite side with combined simple queries and if not enough even process data in ts/c++ directly
- Postgresql is reference implementation, for default server mode, sqlite is for easy deployment of small setups.
- Server-Admin must decide on first run which to use, currently no migration between sqlite and postgresl db data sets.
- Test both DB-Modes in tests

## HTTP-Server

- ElysiaJs + Eden Treaty on TypeScript-Side, the route definitions belong into packages/shared, so frontend-core, frontend and admin cann import the types
- h2o on C++ Side, configured to not allocate/free memory at all, it should start with enough memory which will be frequently reused.
- Routes are additional described in contracts/server as json for easy test/compare of both implementations

## Config

- env for variables which are needed on startup (db, ports, etc.)
- secrets for production using os native Secret stores (like systemd credential on linux)
- secrets on dev via env
- const settings into const, dynamic settings into settings table, editable from admin frontend, but only from admin, no separat rights created for this

## Setup

- bun + turborepo + tsgo + biome for ts side
- zig as C/C++ Compiler and package manager for compatible third party libs
- zig as compiler for shared-native module for ts
- clang-format for linting C/C++ code
- google test for testing the C/C++ code

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

- Rights defined in Code as Enums string -> number mapping, optimized for bit-operator check, enum per domain
- default roleRights in code, admin is allowed everything, user has a explicit default set of allowed things, the same as moderator and ai-moderator
- roleRights entries in db can inherit from default roles to extend or even restrict default roles, default roles cannot be overwritten
- which Roles have which rights default in code, extra roles (with inheritance from default roles) in db, stored as string, ignore unknown string from db, log as warning
- Global Cache for roleRights, max ttl 10 Minutes, invalidate if admin edit a role
- Compact Rights, defined by domain layer, max 64 rights per domain for using bits in bigint (uint64)
- bitset enum in runtime, string in db
- group permission free routes in on file (like login or view community infos)
- Every non-public request with jwt token create a new one, so as long the user is active, his session timeout will constantly moving,
but the session-context will be deleted after 10 minutes nevertheless (hard timeout)

roles: id, parent_role(optional, varchar), role (varchar), description (text)
parent_role als string, da die default roles in Code definiert sind

roleRights: id, role_id, domain, right (varchar), created_at
on right per Row

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

## Suggested source organization

Organize primarily by **business domain**, not by technical layer or database table.
The exact top-level domains should follow business capabilities rather than blindly mirroring database tables.

## Session implementation boundary

The generic session mechanism belongs near the application layer:
But the semantics of cached domain data belong to their domain.
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

This keeps the consistency strategy visible in business code without coupling the business logic directly to MariaDB/Drizzle details.

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
