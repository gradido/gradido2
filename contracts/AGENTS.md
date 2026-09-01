# AGENTS.md — contracts/

## What this directory is

`contracts/` is the language-independent description of behavior that **both**
implementations must satisfy — TypeScript in `packages/`, C in `fast-servers/`.

It is not documentation of the TypeScript code. It is the agreement the fast path is
tested against, and the place where a behavioral question is answered once instead of
twice.

Read `../AGENTS.md` first for the project rules, `../Architecture.md` for the reasoning.

---

## The one rule that makes this work

**Every value here must survive being read by a C parser and a JavaScript parser and come
out identical.**

Everything below follows from that sentence.

### Numbers are decimal strings

```json
"value": "10000000"
```

Never a bare JSON number. JSON numbers above 2^53 lose precision in JavaScript, C parsers
disagree about integer-vs-double, and a contract whose values change depending on who reads
it is worse than no contract. One rule, no exceptions, no judgement calls at the call site.

### Every value carries its type

```json
{ "type": "uint64", "unit": "gradido_cent", "value": "10000000" }
```

C needs the width; TypeScript needs to know whether it is `bigint` or `number`. Neither can
infer it from `"10000000"`. The `type` field is what makes generated code possible.

### Bytes are lowercase hex, timestamps are explicit

```json
{ "type": "bytes", "value": "ffd8" }
{ "type": "int64", "unit": "unix_seconds", "value": "1620927991" }
```

No `Date` strings, no locale, no timezone. A unit field says what the number counts.

### Time is UTC everywhere behind the frontend

Database and wire carry UTC only, as milliseconds since the Unix epoch. A local time exists
in exactly one place: the browser, at the moment of rendering. Nothing behind the frontend
knows the user's zone.

`types/Timestamp.json` is normative — read it before touching any column or field that
holds an instant. It also carries the one genuinely open question: which zone a calendar
based contribution cycle starts in.

### Names are stable forever

A name or a numeric code in this directory is an identifier that other code and older
databases depend on. **Rename nothing, renumber nothing, reuse nothing.** To retire an
entry, mark it `"deprecated": true` and leave it in place.

---

## Type vocabulary

Use these and nothing else. Each maps to both target databases and both languages.

| contract type | PostgreSQL | SQLite | C | TypeScript |
|---|---|---|---|---|
| `uint32` | `integer` | `INTEGER` | `uint32_t` | `number` |
| `uint64` | `bigint` | `INTEGER` | `uint64_t` | `bigint` |
| `int64` | `bigint` | `INTEGER` | `int64_t` | `bigint` |
| `bool` | `boolean` | `INTEGER` | `bool` | `boolean` |
| `char(n)` | `char(n)` | `TEXT` | `char[n+1]` | `string` |
| `varchar(n)` | `varchar(n)` | `TEXT` | `char[n+1]` | `string` |
| `text` | `text` | `TEXT` | `char*` | `string` |
| `uuid` | `uuid` | `TEXT` | `uint8_t[16]` | `string` |
| `timestamp_ms` | `timestamptz(3)` | `INTEGER` | `int64_t` | `Date` |

`timestamp_ms` is UTC milliseconds. PostgreSQL uses `timestamptz`, never bare `timestamp` —
a bare one discards the zone on write and the two implementations then read back different
instants. See `types/Timestamp.json`.
| `bytes(n)` | `bytea` | `BLOB` | `uint8_t[n]` | `Uint8Array` |
| `gradido_cent` | `bigint` | `INTEGER` | `int64_t` | `bigint` |

`gradido_cent` is the money type: an integer in 1/10000 GDD. It is never a float, never a
decimal string in the database, and never computed on with `number`. See `../AGENTS.md`,
*Amounts*.

**Identifiers are `uint64`, everywhere, with no exceptions.** Primary keys and every foreign
key that points at one. Not because 32 bits are too few for users — they are not — but
because legacy mixes the two widths (`users.id` is `int unsigned`, `users.referrer_id`
pointing at it is `bigint unsigned`), and a foreign key narrower than its target truncates
silently in C. One width removes the whole failure class instead of reviewing for it. The
cost is four bytes per row in PostgreSQL, nothing in SQLite, and nothing on the wire, where
every number is a decimal string already.

A column ending in `_id` is not automatically an identifier — `email_opt_in_type_id` holds
an enum value and stays `uint32`. Look at what it references, not at what it is called.

Anything not in this table is a decision, not a detail — raise it rather than inventing a
type. The legacy `geometry` column is the open case, see `db/users.json`.

---

## File shapes

Every file carries an envelope so a loader can check what it is reading:

```json
{ "contractVersion": 1, "kind": "...", "...": "..." }
```

`kind` is one of `const`, `enum`, `convention`, `errors`, `logging`, `settings`, `rights`,
`table`, `route`, `test-vectors`.

`convention` is for cross-cutting behavior that is neither a value nor a shape — how time is
represented, how strings are compared. It lives in `types/` beside the enums.

### const.json

One flat map, keyed by name. `group` is for humans and for grouping test output; `source`
records where the value came from so the legacy original stays findable.

### settings.json

The same shape as `const.json` and for the same kind of value — the difference is when it
changes. A constant changes with a release; a setting changes while the server runs, from the
admin frontend. `db/settings.json` and `db/user_settings.json` hold the rows; this file holds
what a row *means*, and without it a row is an untyped string that the two implementations
parse differently.

Each entry adds `scope` (`instance` or `user`, which decides the table), a `default` in the
same text form the database stores, and the bounds — `min`, `max`, `nullable` — that both
implementations reject the same value by. **`default` is authoritative:** an absent row means
exactly this value, and no implementation re-derives it from an environment variable.

The exclusions carry their reason with them, in the file's `notes`. A value that must exist
before the database is open stays in env, a value whose leak is an incident stays in the
secret store, and a value that already has a home — a column on `communities`, a function in
`shared-native` — does not get a second one here.

### rights.json

The registry of everything the auth system checks: every right, the domain it belongs to,
the **bit** it occupies in that domain's `uint64` mask, and the default roles that hold it.
`db/role_rights.json` stores strings out of this file and the `rights` arrays in `server/**`
name them.

A bit is a value, so the same rule applies as everywhere else here: it is written down
explicitly, never derived from array order, never renumbered, never reused. The domain is
part of the identity — a bit means nothing without it — so **moving a right into a
better-fitting domain later is a renumbering** and is not allowed either. Sixty-four rights
per domain is the ceiling, because a domain's mask is one `uint64`.

The default roles live here and **only** here — nothing seeds them into the database, which is
why `roles.parent_role` and `user_roles.role` hold names rather than foreign keys. `ADMIN`
carries `"all": true` and is never enumerated; the others are read off the `roles` array of
each right.

Two contract tests keep it honest: every right named in `server/**` exists here and every
right here is named by a route, and no domain holds more than 64.

### types/&lt;Name&gt;.json — one file per type

Enums declare `representation` (`string` or an integer type) and list values explicitly.
**Never derive a value from declaration order.** Order in a file is presentation; a value
is a promise.

`unknownValuePolicy` states what an implementation does with a value it does not know —
this is behavior, so it belongs in the contract, not in each implementation's judgement.

### errors/&lt;domain&gt;.json

Each error has a stable `code`, a `name`, a `messageTemplate` with `{named}` placeholders,
and typed `parameters`. Both implementations format the same string from the same template.

Code ranges, so two domains never collide:

```text
1000–1999  database
2000–2999  domain / validation
3000–3999  api / mutation
4000–4999  auth
5000–5999  federation
```

Legacy had no numeric codes — it threw classes. The codes here are assigned by gradido2 and
are new. That is exactly why they must never be renumbered afterwards.

### db/&lt;table&gt;.json — one file per table

Describes the **target** schema for gradido2 (PostgreSQL/SQLite), informed by legacy
(MariaDB). It is not a copy of the legacy DDL: legacy types that gradido2 will not keep are
marked `"open": "..."` rather than transcribed.

Indexes are part of the contract, not an optimization detail. The legacy entities did not
declare the index on `users.gradido_id` — a migration did — and a benchmark built from the
entities alone read 1.9× too slow without noticing. If an index matters for correctness of
performance, it is written here.

### logging.json

The envelope every log line carries, the numeric levels, the closed set of categories, the
event id grammar, the seed events and the redaction list.

Two things there are easy to get wrong. **Tests compare structure, never `msg`** — the human
sentence is not contractable across two languages and trying makes the suite unkeepable.
And **Pino's default `pid` and `hostname` must be switched off**, or two identical runs
compare unequal before anything else is checked.

Legacy's per-class log4js categories are deliberately not carried over.

When a legacy column holds something the vocabulary above has no entry for, the column keeps
a descriptive type name and carries `"open": "..."` saying what has to be decided. It is never
guessed into the nearest available type — five such columns exist and they are listed in the
Status section below.

### server/&lt;server&gt;/&lt;model&gt;.json — one file per model

Route definitions, split by the server that serves them: `server/backend/` and
`server/federation/`. One file per **model**, not per legacy resolver — `CreationGroupResolver`
and `UserCreationGroupResolver` are one model and one file.

A route name is `&lt;model&gt;.&lt;action&gt;` in camelCase, and the path is
`/&lt;model&gt;/&lt;action&gt;` in kebab-case. Both are derived from the legacy GraphQL field
name with the model taken out of the verb: `createContributionLink` becomes
`contributionLink.create` at `POST /contribution-link/create`. Every path is exactly two
segments, so a router — including a C one — matches on a fixed shape rather than on a
pattern. `method` is `GET` for what was a query and `POST` for what was a mutation.

Federation files carry `mount: "/api/{apiVersion}"` and the `apiVersions` that expose them;
the route paths themselves are relative to that mount. Legacy 1_1 reuses 1_0's resolvers
except for `PublicKey`, and drops `BlockchainNotification` and `Disbursement` — that is in
the files, not inferred.

`auth` is `public`, `session` or `federation-handshake`. `roles` lists every legacy role
holding the route's `rights`; `public` means the unauthorized role holds them, i.e. legacy's
`INALIENABLE_RIGHTS`. Rights are recorded without the `RIGHTS.` prefix.

Request and response fields carry both the contract `type` and the `legacyType` they came
from. Where legacy used a type the vocabulary above has no answer for, `type` is `null` and
`open` says so — an unanswered question stays visible instead of being guessed into a shape.
`note` is the opposite case: the type is decided, but something about it is worth knowing
(`publisherId` is an Elopage number, not a gradido row id).

A route contracted here that the fast path has not implemented answers `ROUTE_NOT_IMPLEMENTED`
(`errors/api.json`) on a fast-path deployment, and is never proxied to TypeScript — see
`../Architecture.md`, *One implementation per deployment*. What is contracted here is therefore
the full route set of the reference implementation, not of whichever path is deployed.

`legacy` on each route points back at the resolver, the operation and the field it was
collected from. That block is provenance and follows working rule 6 — a lead, not an
authority. A route gradido2 invents rather than inherits carries `"legacy": null` and the file
carries `"origin": "gradido2"` — `peer.bootstrap` is the first. Such a route has no legacy
behavior to be faithful to, so its request and response are contracted in full instead of
being left open.

### test-vectors/&lt;subject&gt;.json

Input/expected pairs, referencing types and errors by name. Every vector has a stable `id`
so a failure names something. `jwt.json` is the worked example; read it beside this section.

**A subject is one file and two runners.** The file is the authority and neither runner is:

```text
contracts/test-vectors/<subject>.json          the vectors
packages/contract-tests/src/<subject>.test.ts  run against the TypeScript path
fast-servers/tests/contract/test_<subject>_contract.cpp   run against the C path
```

Each runner reads the file and is measured against it, never against the other implementation.
That is what makes a disagreement one named vector instead of two suites that are both green,
and it is why a runner may not skip a vector it does not like — a vector nobody runs is a
disagreement nobody reports. Both loaders (`src/vectors.ts` and `tests/contract/vectors.hpp`)
therefore check the same three things before any vector runs: the declared `count` is the number
actually read, every `id` is unique, and every `id` names its subject.

**Field types are declared once, in `fields`, not on every value.** The rule at the top of this
file still holds — a number is a decimal string and its type is written down — but a vector file
carries hundreds of values of the same handful of fields, and a `{ "type": ..., "value": ... }`
object around each would triple the file for no information a reader or a generator does not
already have. `fields` maps a field name (dotted for a nested one) to its `type`, `unit` and
`nullable`. Values that stand alone rather than in a vector — a shared secret, a bound in
`rules` — keep the per-value form, because there is one of each.

**Every vector is wrong in at most one way.** A token that is both expired and misaddressed only
pins whichever check happens to run first, and the two implementations do not check in the same
order. Where a vector needs two things wrong at once, it needs two vectors.

**Contract only what is observable.** `jwt.json` contracts whether a token is accepted and the
claim that comes out of it — not *which* refusal it was, because every refusal is one 401 with
one body and a caller cannot tell them apart. The reason each implementation reports lives under
its own key (`c.result`) and is asserted by that implementation's runner alone. Widen the
contract to something neither side can observe and it becomes a test of an accident.

**A divergence is declared, not omitted.** Where one implementation cannot meet `expect`, the
vector carries a block named after it — `"typescript": { "accepted": ..., "why": ... }` — and
that runner asserts the divergence *still happens*. A gap that closes upstream then fails the
suite until the block is deleted, so the list of known disagreements cannot rot into a list of
things somebody once believed. Both runners also assert the whole list, so adding one is a
deliberate edit in three places rather than a quiet `skip`.

A vector may not disagree with itself, and the runners check that too: an acceptance that names
no value, or a refusal that names one, would be read one way by a runner that looks at
`accepted` and another way by one that looks at the value.

**Derived fields carry a regenerator and are still verified.** `jwt.json` stores each token's
signed bytes, which nobody can recompute by hand;
`bun run regen:vectors` (`scripts/regen-contract-vectors.ts`) writes them back. The suite does not trust it —
it recomputes the same tokens and fails on a disagreement, so a payload edited without
regenerating is caught rather than believed.

---

## Working rules

1. **Behavior changes and its contract change in the same commit.** A contract that lags is
   worse than none, because tests then certify the wrong thing.
2. **When the legacy behavior is unclear, look it up in the legacy repository and write the
   answer here.** That is the point of this directory: the question gets answered once.
3. **Do not put frontend-only values here.** Route anchors, CSS class prefixes and login
   route names concern one implementation, not both. The test for inclusion is: *would a
   disagreement between the two backends be a bug?*
4. **Infrastructure is per implementation — but its observable output may not be.** Pool
   sizes, ports and thread counts belong to each implementation. Log *output* does not:
   `Architecture.md` requires both to emit the same JSON, so the envelope, levels,
   categories, event ids and redaction rules are contracted in `logging.json`. The test is
   the same as always — would a disagreement be a bug?
5. **What `shared-native` exports does not belong here.** Decay constants, the gregorian
   year, the crypto sizes — both implementations import them from the same C library, so a
   copy in `const.json` would be a second source of truth for exactly the values that must
   never diverge. Reference the FFI function instead: `grdc_decay_respite_cent()`,
   `getDecayStartTime()`. This directory covers what the two implementations would
   otherwise each decide for themselves.
6. **Provenance:** `source` points at the legacy file a value came from. It may go stale as
   legacy moves — it is a lead, not an authority. The value here is the authority.
7. **Legacy is a source of requirements, not of designs.** Some of it is marked
   `AI-GENERATED — not an architecture reference` in its own header. Take the rule it
   encodes; leave the schema it chose.

---

## Status

| Area | State |
|---|---|
| `const.json` | from legacy `shared/src/const`. Values exported by `shared-native` are deliberately absent, see working rule 5 |
| `types/` | 20 enums — 18 from legacy `shared`, `database` and the backend, plus `RightDomain` and `ScopeDimension`, which gradido2 invents — and the `Timestamp` and `PasswordHash` conventions |
| `errors/` | database, domain and mutation errors; codes newly assigned |
| `db/` | **28 tables — every one legacy has except `user_creation_groups`**, which could never hold more than one row and becomes the member setting `main_creation_group` — plus `settings`, `user_settings`, `roles`, `role_rights` and `user_role_scopes`, which gradido2 invents and legacy has no counterpart for. Column types, defaults, keys and indexes are decided; what is not is collected as `open` on the column or on the table |
| `settings.json` | **24 runtime keys** — 21 instance settings from legacy's config, and in `user_settings.json` 3 member settings: 2 leaving `db/users.json` and `main_creation_group`, which replaces a table. Types, defaults and bounds are decided; the three publish-name enums are not |
| `rights.json` | **79 rights in 10 domains**, each with its bit, plus the 6 default roles and their sets, the dimensions each domain exposes and the one evaluation rule both implementations share. Read off the `rights` and `roles` arrays of the routes; one grant and the role-name spelling are `open` |
| `logging.json` | envelope, levels, 11 categories, 13 seed events, redaction |
| `server/` | **139 routes** — 134 collected from legacy (121 backend, 13 federation), plus `peer.bootstrap` and the four in `backend/role.json`, which legacy has no counterpart for. Three legacy routes are marked `deprecated`, superseded by those four. Names, paths, methods, rights and roles are decided; request fields are filled in from the legacy arg classes, response shapes are not, except on `peer.bootstrap` which is new and therefore fully contracted |
| `test-vectors/` | **`jwt.json`: 37 vectors** for HS256 verification, run against `jwt.c` and against jose, with one declared divergence (a list-valued `aud`). The shape and the two runners are the pattern the subjects below are written to; everything else is still missing |

Next, in this order:

1. **Test vectors for decay and GradidoUnit.** The determinism-critical path has the
   highest cost of divergence and the lowest cost of testing. `shared-native` already has
   the implementation and `../gradido/shared-native/tests/calculateDecay.test.js` has cases
   to lift. Both sides call the same C through different bindings, so unlike `jwt` there is no
   second implementation to disagree with — what a vector pins there is the binding and the
   arithmetic around it, not two parsers.
2. **The routes behind the settings tables.** `settings.json` and the two tables are written;
   nothing reads or writes them yet. What is missing is the admin route that changes a value —
   which is also where the staleness bound in `db/settings.json` has to be answered, because
   an admin frontend that appears not to work is the first thing anyone will notice.
3. **The five columns whose type has no answer.** `users.location` and `communities.location`
   (MariaDB geometry, and neither PostgreSQL without PostGIS nor SQLite has one),
   `user_avatars.avatar_small` and `avatar_full` (a variable-length blob, which the vocabulary
   above has no entry for — and which may not need one, if the column becomes an object key),
   and `crea_records.hours` (the only floating point value in the schema; a count of minutes
   would remove it). Each is written as `open` on its column.
4. **Response shapes.** The routes exist, but every response that is not a scalar still
   says `open: shape of X not contracted yet`. Those are the legacy `@ObjectType` models in
   `../gradido/backend/src/graphql/model/`. Along with them, the four request types the
   vocabulary has no answer for yet: `Location`, `PublishNameType`,
   `GmsPublishLocationType`, `CreaBatchContribution` — and legacy's `Float` return on
   `gdt.getBalance`.
5. **Media objects**, once the storage decision is settled (see `../Architecture.md`,
   *Media storage*). What will need contracting is not the backend but the behavior around
   it: the object key derived from a user, which rendition may be shown to whom, accepted
   content types and size bounds. Legacy's avatar limits were removed from `const.json`
   with the design they belonged to.
