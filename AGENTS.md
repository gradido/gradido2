# AGENTS.md — Gradido2

## Purpose

Operational guide for AI coding agents working on Gradido2.

**Read `Architecture.md` for the detailed architecture and rationale.**

When existing code and documentation appear to disagree, inspect the surrounding code and tests before introducing a new abstraction. Do not silently invent a new architecture.

---

## 1. Core rules

### TypeScript is the reference implementation

TypeScript defines the current reference business behavior.

When implementing corresponding functionality elsewhere, inspect the TypeScript behavior first.

### C++ is the High-Performance backend

C++ is an independent implementation of the same business/domain behavior and may lag behind TypeScript.

When changing TypeScript:

1. identify whether business behavior changed;
2. locate the corresponding C++ domain path;
3. assess whether C++ is affected;
4. update C++ only when required.

Do **not** force artificial TS/C++ parity.

When implementing C++, preserve TypeScript's business semantics but use idiomatic C++.

### C is the low-level primitive layer

Keep small, explicit, reusable, ABI-oriented and memory-sensitive primitives in C where appropriate.

Examples include `hostmem`, arena allocators, specialized vectors, fixed-point arithmetic, and similar low-level libraries.

C++ may use these libraries.

Do not rewrite C libraries in C++ merely for language uniformity.

---

## 2. Domain structure

Organize business code by domain, not by global technical layers.
The same business-domain structure should exist in TypeScript and C++ where both implementations exist.

For example:

```text
TypeScript:
domain/community/interactions/add-member.ts

C++:
domain/community/interactions/add-member.cpp
```

The structure is a navigation/correspondence system, not a requirement for identical implementation details.

### Empty directories are intentional

A new Community Interaction belongs in:

```text
domain/community/interactions/
```

Do not create an alternative location because the directory was previously empty.

---

## 3. DCI

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

Examples:

```text
createTransaction
cancelTransaction
addCommunityMember
renameCommunity
```

Interactions should make the business story readable.

---

## 4. Session / Working Context

The session is a reconstructible, ephemeral working context.

```text
Database = truth
Session  = disposable working view
```

Load data lazily.

Do not eagerly load the complete user state.

Every session field must be safe to lose and reconstruct from persistent data.

A server restart or request routed to another instance must remain correct.

---

## 5. Cache consistency belongs near business logic

Do not hide domain-specific invalidation in a generic global cache manager.

When an Interaction changes data, make its session consequences visible in that Interaction.

For example:

```text
createTransaction
    -> persist transaction
    -> update session.transactions
    -> update session.user.balance
```

For foreign changes, prefer cheap freshness markers:

- version
- generation
- sequence
- cursor

Do not maintain a global list of every affected session unless explicitly required.

---

## 6. Data-specific freshness

Before caching data, determine:

1. who can change it;
2. whether the current user can change it;
3. whether another user can change it;
4. whether it is append-only;
5. whether it can be versioned;
6. whether it can be refreshed incrementally;
7. how fresh it must be.

Typical strategies:

```text
user-owned       -> keep locally, update on own writes
append-only      -> sequence/generation/cursor
foreign-writable -> version/generation validation
static           -> long-lived cache if useful
volatile         -> strict freshness or no cache
```

---

## 7. Own writes vs. foreign writes

For a mutation performed by the current Interaction:

```text
persist
+
update current session
```

Prefer precise updates over invalidating data that the operation already knows how to update.

For changes made elsewhere:

```text
version/generation changes
        ↓
session detects stale state on access
        ↓
refresh
```

Avoid distributed invalidation bookkeeping.

---

## 8. Repository boundary

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

## 9. Do not over-engineer

Do not introduce Redis, sticky sessions, distributed invalidation, generic cache managers, speculative service layers, or artificial TS/C++ synchronization without a concrete requirement.

Prefer the smallest structure that expresses the actual business requirement.

> **Avoid work first. Optimize remaining work second.**

---

## 10. C++ rules

C++ should mirror the TypeScript **business/domain structure**, not necessarily its implementation technique.

Use idiomatic C++ where appropriate.

RAII, value types, templates, STL containers, classes, and C libraries are all allowed when they provide a real benefit.

Do not introduce OOP merely to make C++ look like TypeScript.

---

## 11. C rules

C is appropriate for small, explicit, reusable low-level primitives where memory layout, ownership, predictable allocation, ABI, or FFI matter.

C may lag behind TypeScript and C++.

---

## 12. Change workflow

For a business behavior change:

```text
1. Identify domain.
2. Inspect existing Data.
3. Inspect existing Logic.
4. Identify/modify the Interaction.
5. Determine session impact.
6. Determine freshness/invalidation behavior.
7. Update TypeScript reference behavior.
8. Check corresponding C++ implementation.
9. Update C++ only if required.
10. Check C only if the change crosses a C primitive boundary.
```

For a new feature, start from the business operation, not from infrastructure.

---

## 13. Final safety check

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
Did C++ preserve semantics while remaining independently optimized?
```

If not, reconsider the design.

> **Make business behavior obvious.**
>
> **Make consistency rules visible.**
>
> **Make RAM state cheap to lose.**
>
> **Let TypeScript define reference semantics.**
>
> **Let C++ optimize the backend.**
>
> **Let C provide small, reusable low-level primitives.**
