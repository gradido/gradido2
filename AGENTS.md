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

When changing TypeScript: identify whether business behavior changed, locate the
corresponding domain path in `fast-servers/`, assess whether it is affected, update it only
when required. Do **not** force artificial parity.

> **Working in `fast-servers/`? Read `fast-servers/AGENTS.md` and
> `fast-servers/Architecture.md`.** The house dialect for C, the `extern "C"` rule for C++
> leaf modules, the sanitizer requirements and the session cache invariants live there.
> They are not repeated here.
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
                   has its own AGENTS.md and Architecture.md — read them before
                   writing C, they are not summarised here
contracts/         language-independent JSON contracts, see section 5
                   has its own AGENTS.md for the file formats
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

## 4. Four kinds of code

Before writing anything, decide which of these it is:

| Kind | Lives | Mirrored |
|---|---|---|
| **determinism-critical** | once, in C, in `shared-native` | **never** |
| **single-implementation** | once, in C, in `blockchain-core` | **never** |
| **protocol-defined** | twice, on a conformant library per language | yes — by the protocol, not by us |
| **domain / business** | TypeScript reference | yes, C follows |
| **infrastructure** | each implementation its own | no |

`shared-native` exists for determinism, not for speed. The selection rule is the opposite of
the performance one:

```text
performance-motivated   -> no. The crossing costs more than it saves.
determinism-motivated   -> always, whatever it costs.
single-implementation   -> yes, when the alternative is writing a protocol or an
                           algorithm twice and hoping the two agree.
protocol-defined        -> twice, when no one language has the library and the
                           protocol itself is what keeps the halves together.
```

The second case is signing, hashing and transaction serialisation: one C++ library called
from both paths, because two implementations of a wire format disagree without failing a
test.

The fourth case is peer discovery, and it is the only one. libp2p — Kademlia plus transports,
multiplexing, NAT traversal, peer store, identify — exists in `rust-libp2p` and `js-libp2p`
and nowhere else that both paths can reach; writing it in C would be the project instead of
Gradido. So:

```text
packages/dht-node       js-libp2p, TypeScript. The reference path.
fast-servers/dht-node   rust-libp2p as a static library behind extern "C".
```

Each path runs its own node. The fast path does not read rows a TypeScript process writes —
on a small server that second runtime is a whole process's worth of RAM spent on peer
discovery, and on a high-load server it would make the fast path merely rearranged rather than
droppable. That is the reason Rust is in this repository at all.

Rust is a leaf language on the fast path in exactly the sense C++ already is: one module, one
`extern "C"` surface, no Rust type across the boundary, no application logic behind it. Do not
put Rust anywhere else, and do not add a second Rust module without changing this document
first.

Two peer-discovery implementations that drift apart stop finding each other without failing
anything, so the gate is an **interop test** in CI, not a contract vector — there is no value
to compare, only a behavior between two running processes. Both libraries are pinned; neither
is raised without that test green on the pair.

The DHT node does not touch the database, on either path. It reports what it discovers;
persisting that is an Interaction's job, through a Repository. The sweep is O(communities)
every 20 seconds, so both nodes keep the peer state inside the library and hand out only what
changed — the boundary then scales with the number of changes, not with the number of
communities.

Transports are TCP + QUIC with circuit relay v2 as the fallback; WebRTC stays off, because
Gradido's peers are servers and the frontend never joins the DHT. Bootstrap is one community
URL, `gdd.gradido.net` by default, over the public `peer.bootstrap` route. What that route
returns is a sample of currently reachable peers and a **hint, not a trust decision** — a peer
is verified when it is contacted, never when it is named. Do not write a bootstrap answer into
`federated_communities` unverified.

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

**Read `contracts/AGENTS.md` before adding or changing anything there** — the file formats
are designed so that a C parser and a JavaScript parser read every value identically, and
the rules that achieve that are not guessable (all numbers are decimal strings, every value
carries its type, names and codes are permanent).

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

## 12. Toolchain

Pinned versions, so they are not guessed:

```text
zig    pinned by c-cpp-zig-build, which builds shared-native.
       One place, not three — legacy pinned 0.15.2 in
       build_helper/const.ts while ../h20Test asked for 0.15.1.
       Read the version out of the package; do not guess it and do
       not carry a number from an older repository into this one.
```

`bun install` followed by `turbo backend#start` is enough — do not install a toolchain
manually and do not add one to the instructions.

Record Elysia idioms that keep being reinvented here as well; h2o belongs in
`fast-servers/AGENTS.md`.

---

## 13. Dependencies

Gradido moves money. Every package added here runs with the same rights as the code that
handles balances, and the npm registry has seen a steady run of compromised releases —
usually a maintainer account taken over and a malicious patch version published, caught
within days but shipped to everyone who resolved a range in the meantime.

**Pin the exact version.** No `^`, no `~`, no ranges — in `package.json` and in
`build.zig.zon` alike.

```json
"jose": "5.10.0"      not  "^5.10.0"
```

**Commit the lockfile.** Pinning direct dependencies without one leaves every transitive
dependency floating, and that is where these attacks usually land. CI installs with
`--frozen-lockfile` so a drifted tree fails the build instead of being installed.

> Note for whoever sets this up: the root `.gitignore` currently lists `bun.lockb`. That
> line is correct for a library and wrong for an application — it needs to go.

**Do not take the newest version.** A malicious release is typically pulled within days of
publication, so age is a cheap filter that costs nothing but patience. Prefer a version that
has been available for about a month unless a security fix says otherwise. Being one minor
version behind is not a risk; being first to install a compromised patch is.

The exception is a package published by this project — `c-cpp-zig-build` and anything that
follows it. Waiting a month on your own release protects against nothing, because the risk
the waiting period covers is a stranger's compromised account, not your own publish. Pin it
exactly all the same.

**Check before adding.** Who publishes it, when it was last released, whether the repository
matches the package, how many dependencies it drags in, whether it runs install scripts.
Install scripts are the execution vector — `--ignore-scripts` should be the default, with
exceptions listed and justified.

**The strongest measure is not adding it.** This repository already has the example:
dropping `jose` for the built-in `node:crypto` gave +54 % throughput at half the CPU per
request, in forty lines, and removed a dependency from the authentication path rather than
adding one. Fewer packages is better security and, here, better performance.

For zig, dependencies in `build.zig.zon` are pinned to fixed commits and carry a hash, and
`c-cpp-zig-build` verifies the Zig and Node archives it downloads against the SHA-256 the
upstream projects publish. Same policy, already in place on that side.

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
Is every new dependency pinned, aged, and in the committed lockfile?
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
