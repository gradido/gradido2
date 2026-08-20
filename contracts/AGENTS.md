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

`kind` is one of `const`, `enum`, `convention`, `errors`, `logging`, `table`, `route`,
`test-vectors`.

`convention` is for cross-cutting behavior that is neither a value nor a shape — how time is
represented, how strings are compared. It lives in `types/` beside the enums.

### const.json

One flat map, keyed by name. `group` is for humans and for grouping test output; `source`
records where the value came from so the legacy original stays findable.

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

### test-vectors/&lt;subject&gt;.json

Input/expected pairs, referencing types and errors by name. Every vector has a stable `id`
so a failure names something.

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
| `types/` | 13 enums from legacy `shared` and `database`, plus the `Timestamp` convention |
| `errors/` | database, domain and mutation errors; codes newly assigned |
| `db/` | **6 of ~24 tables** — `users`, `user_contacts`, `user_roles`, `transactions`, `contributions`, `transaction_links` |
| `logging.json` | envelope, levels, 10 categories, 8 seed events, redaction |
| `server/` | empty — routes are not designed yet |
| `test-vectors/` | empty — **the most valuable missing piece** |

Next, in this order:

1. **Test vectors for decay and GradidoUnit.** The determinism-critical path has the
   highest cost of divergence and the lowest cost of testing. `shared-native` already has
   the implementation and `../gradido/shared-native/tests/calculateDecay.test.js` has cases
   to lift.
2. The remaining tables, driven by whichever domain is being rebuilt — not all 24 up front.
3. Routes, once the first ones exist.
4. **Media objects**, once the storage decision is settled (see `../Architecture.md`,
   *Media storage*). What will need contracting is not the backend but the behavior around
   it: the object key derived from a user, which rendition may be shown to whom, accepted
   content types and size bounds. Legacy's avatar limits were removed from `const.json`
   with the design they belonged to.
