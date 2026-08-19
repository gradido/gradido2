# AGENTS.md — Gradido2

## Purpose

Operational guide for AI coding agents working on Gradido2.

**Read `Architecture.md` for the detailed architecture and rationale.** This file states
the rules; Architecture.md explains why they exist. When a rule here needs justification,
it links there instead of repeating it.

When existing code and documentation appear to disagree, inspect the surrounding code and tests before introducing a new abstraction. Do not silently invent a new architecture.

---

## 0. State of the repository

Gradido2 is a **rebuild of gradido legacy on a new stack**, not a refactoring of its code.
No legacy code is carried over — but the **complete feature set of gradido legacy is the
target scope**. Most directories are still empty because the rebuild has barely started,
not because the scope is small.

`https://github.com/gradido/gradido` is therefore not a discarded predecessor but an active resource:

- the **behavioral reference** for every ported feature. When a rebuilt feature's behavior
  is unclear, the answer is in the legacy implementation, and the answer belongs in
  `contracts/test-vectors` once found.
- `https://github.com/gradido/gradido/shared-native` already contains determinism-critical C — decay, GradidoUnit
  arithmetic, signing, transaction types and validation. Reuse it, do not reimplement it.

The `package.json` files currently in the tree are leftovers from an earlier attempt.
**Do not treat them as a reference for dependencies, package names, or the tech stack.**
Where they disagree with `Architecture.md` — e.g. tRPC vs. Eden Treaty, zod vs. valibot,
mysql2 vs. PostgreSQL/SQLite — `Architecture.md` is correct and the `package.json` is
obsolete.

The toolchain (bun, turborepo, tsgo, biome, zig, google test) is described in
`Architecture.md`, but not all of it is configured yet. Before running or assuming a
build/test/lint command, check what actually exists in the repository. Do not invent
scripts.

---

## 1. Core rules

### TypeScript is the reference implementation

TypeScript defines the current reference business behavior.

When implementing corresponding functionality elsewhere, inspect the TypeScript behavior first.

It is normative because it is the path that survives without the author, not because it is
the fastest or the best-written one. See `Architecture.md`, *Why two implementations*.

Two rules follow, and neither is negotiable:

- **No feature originates in the fast path.** If behavior exists only in C, it has silently
  removed itself from the fallback.
- **The fast path must be droppable, not merely removable.** Running without it must need no
  code change: no shared state, no route only it serves, no role only it fills.

### C is the fast implementation

`fast-servers/` is C: h2o, request path, session, repositories. It is an independent
implementation of the same business behavior and may lag behind TypeScript.

When changing TypeScript:

1. identify whether business behavior changed;
2. locate the corresponding domain path in `fast-servers/`;
3. assess whether it is affected;
4. update it only when required.

Do **not** force artificial parity.

Preserve TypeScript's business semantics, but write idiomatic C.

House dialect, so that review stays uniform and generated code stays checkable:

```text
- no malloc in the request path, arena only
- fixed buffer sizes with an explicit bounds check;
  overflow answers 500, never truncates
- one ownership convention, describable in one line
- function pointers at registration only, not in the data flow
```

### C++ is for leaf modules only

C++ is justified by a library without a C equivalent — `gradido-blockchain-core`, signing,
hashing. Not by the convenience of a container: `std::unordered_map` allocates per insert
and is the wrong structure for a server with no allocation per request. Use open addressing
with a preallocated table, or a sorted array for ordered access.

A C++ module:

```text
- exports an extern "C" header and nothing else
- lets no C++ type cross the module boundary
- is compiled with -fno-exceptions -fno-rtti
```

The reason is concrete: an exception propagating into h2o's C event loop is undefined
behavior.

### zig builds, it does not implement

zig is the build system and cross compiler. No application code — its API still moves
between versions. Pinned versions belong in this file, see *Toolchain*.

---

## 2. Where code goes

```text
packages/          TypeScript, reference implementation
  backend          runnable HTTP server: routes, wiring, startup
  backend-core     backend domain code: data, logic, interactions, repositories
  federation       federation server
  admin            admin frontend
  frontend         user frontend
  frontend-core    UI code shared by admin and frontend
  shared           code shared by frontend and backend: route definitions, schemas
  shared-native    determinism-critical C, called via N-API and linked by fast-servers

fast-servers/      C, mirrors the domain structure of packages/
contracts/         language-independent JSON contracts, see section 5
```

Business logic belongs in the `-core` packages. The packages next to them are the
deployable applications that wire that logic up.

Route definitions belong in `packages/shared` so frontend, admin and frontend-core can
import their types via Eden Treaty.

---

## 3. Domain structure

Organize business code by domain, not by global technical layers.
The same business-domain structure should exist in both implementations where both exist.

For example:

```text
TypeScript:
domain/community/interactions/add-member.ts

C:
domain/community/interactions/add-member.c
```

The structure is a navigation/correspondence system, not a requirement for identical implementation details.

### Empty directories are intentional

A new Community Interaction belongs in:

```text
domain/community/interactions/
```

Do not create an alternative location because the directory was previously empty.

---

## 4. Three kinds of code

Before writing anything, decide which of these it is:

| Kind | Lives | Mirrored |
|---|---|---|
| **determinism-critical** | once, in C, in `shared-native` | **never** |
| **domain / business** | TypeScript reference | yes, C follows |
| **infrastructure** | each implementation its own | no |

`shared-native` exists for determinism, not for speed. The selection rule is the opposite of
the performance one:

```text
performance-motivated native call  -> no. The crossing costs more than it saves.
determinism-motivated native call  -> always, whatever it costs.
```

Anything moved into `shared-native` needs no mirror and cannot diverge. Prefer it whenever a
differing result would be a *wrong* result rather than a slower one.

### Amounts

Amounts are `bigint` in gdd units, never `number`, on both sides.

```text
add, subtract, multiply   exact in both languages, may be written inline
divide, round, decay,     always through shared-native, in TypeScript and in C alike
parse, format             never reimplemented in either
```

## 5. Contracts

`contracts/` describes behavior both implementations must satisfy: constants, types,
db definitions, routes, error codes, and test vectors.

When you change behavior that both implementations share — a route signature, an error
code, a constant, a schema — update the contract in the same change. The contract is
what the fast path is tested against, not a description of the TypeScript code.

---

## 6. DCI

### Data

Represents domain state: User, Transaction, Community, etc.
use .data.ts as ending

### Logic

Small logic directly operating on data.
use .logic.ts as ending

Examples:

```text
calculateDecay()
calculateBalance()
validateTransaction()
```

Do not create an Interaction merely to wrap a simple calculation.

### Interaction

A recognizable business operation combining context, data, persistence, side effects, or session state.

One file per Interaction, inside the domain's `interactions/` directory, named after the
operation (`add-member.ts`, `create-transaction.ts`). No suffix — the directory already
says what it is.

Examples:

```text
createTransaction
cancelTransaction
addCommunityMember
renameCommunity
```

Interactions should make the business story readable.

---

## 7. Session / Working Context

The session is a reconstructible, ephemeral working context.

```text
Database = truth
Session  = disposable working view
```

Load data lazily.

Do not eagerly load the complete user state.

Every session field must be safe to lose and reconstruct from persistent data.

A server restart or request routed to another instance must remain correct.

The session is dropped after 10 minutes regardless of activity, as a backstop against
cache-invalidation bugs.

---

## 8. Cache consistency belongs near business logic

Do not hide domain-specific invalidation in a generic global cache manager.

When an Interaction changes data, make its session consequences visible in that Interaction.

For example:

```text
createTransaction
    -> persist transaction
    -> update session.transactions
    -> update session.user.balance
```

For changes made by others, prefer cheap freshness markers — version, generation,
sequence, cursor — and let the session notice on access that it is behind. Do not
maintain a global list of every affected session unless explicitly required.

See `Architecture.md`, sections *Consistency model* and *Own writes vs. foreign writes*,
for the reasoning and the per-data-kind strategies.

---

## 9. Before caching anything

Determine:

1. who can change it;
2. whether the current user can change it;
3. whether another user can change it;
4. whether it is append-only;
5. whether it can be versioned;
6. whether it can be refreshed incrementally;
7. how fresh it must be.

Then pick the strategy that matches:

```text
user-owned       -> keep locally, update on own writes
append-only      -> sequence/generation/cursor
foreign-writable -> version/generation validation
static           -> long-lived cache if useful
volatile         -> strict freshness or no cache
```

One cache policy for every kind of data is the wrong answer.

---

## 10. Repository boundary

Prefer:

```text
Interaction
    ↓
Repository
    ↓
Database
```

The Interaction decides **when** data is needed.

The Repository handles **how** it is loaded/persisted.

Do not move domain-specific consistency decisions into generic infrastructure.

---

## 11. Do not over-engineer

Do not introduce Redis, sticky sessions, distributed invalidation, generic cache managers, speculative service layers, or artificial synchronization between the two implementations without a concrete requirement.

Prefer the smallest structure that expresses the actual business requirement.

> **Avoid work first. Optimize remaining work second.**

---

## 12. Safety net for native code

Not optional, and less so where the code was AI-generated:

```text
ASan + UBSan + TSan in CI, not only locally.
    TSan matters most — the shared session map is the one
    defect class that expert review does not catch.
Fuzzing for every parser touching attacker-supplied bytes: JWT, JSON.
Contract vectors as a merge gate, green on both implementations.
```

---

## 13. Toolchain

Pinned versions, so they are not guessed:

```text
zig    0.15.2 in ../gradido/shared-native (build_helper/const.ts)
       0.15.1 in ../h20Test
       -> the repositories disagree; verify before assuming, and
          record known API confusions here as they are found
```

`shared-native/build.ts` downloads the pinned Zig for the current platform and builds the
native module. `bun install` followed by `turbo backend#start` is enough — do not install a
toolchain manually and do not add one to the instructions.

Record h2o and Elysia idioms that keep being reinvented here as well.

---

## 14. Change workflow

For a business behavior change:

```text
1. Identify domain.
2. Inspect existing Data.
3. Inspect existing Logic.
4. Identify/modify the Interaction.
5. Determine session impact.
6. Determine freshness/invalidation behavior.
7. Decide the kind of code (section 4). Determinism-critical -> shared-native, done.
8. Update TypeScript reference behavior.
9. Update the contract in contracts/ if shared behavior changed.
10. Check the corresponding path in fast-servers/.
11. Update it only if required.
```

For a new feature, start from the business operation, not from infrastructure.

---

## 15. Final safety check

Before finishing, verify:

```text
Can the application recover if all RAM state disappears?
Can a request land on another server instance?
Is the database still authoritative?
Is session state reconstructed lazily?
Is invalidation visible near the relevant business logic?
Does the code live in the correct domain?
Did the change accidentally introduce a generic abstraction?
Did TypeScript remain the reference behavior?
Would the product still work if the fast path were switched off?
Does any behavior now exist only in the fast path?
Did the fast path preserve semantics while remaining independently optimized?
Does contracts/ still describe the actual shared behavior?
Did money arithmetic go through shared-native?
```

If not, reconsider the design.

> **Make business behavior obvious.**
>
> **Make consistency rules visible.**
>
> **Make RAM state cheap to lose.**
>
> **Let TypeScript define reference semantics, so the project survives its author.**
>
> **Let C carry the load, and let it be droppable.**
>
> **Let determinism live in one place only.**
