## CODEBASE_LORE_GLOSSAR.md

Map lore abstractions to their real code concepts.

| Lore | Real |
|---|---|
| Senomagos ("the old market") | `../gradido`, gradido legacy — running, still the behavioral reference |
| The four officials at the gate | Express, Apollo Server 2, type-graphql, TypeORM |
| The Archive | the database — the source of truth. In Senomagos MariaDB, idling at half a core while one JS thread saturates |
| Eight hundred and seventy-seven travellers a day | 877 req/s, the measured per-machine ceiling of the legacy stack |
| The Surveyor | `../h20Test` — the benchmark repository, three stacks on one pipeline |
| The fast road | C on h2o |
| Losing it again at the border | the N-API crossing; it costs more than it saves unless C owns the whole request |
| The question nobody could answer | the real peak load, which no measurement in the repository contains |
| The empty ground, one street plan | gradido2 — a rebuild, no legacy code carried over |
| Sirodunon ("the fort that lasts") | the TypeScript path in `packages/` — normative, maintainable without the author |
| Eporedon ("the city of chariots") | the C path in `fast-servers/` — faster, denser, allowed to lag, must stay droppable |
| One hand built her | the fast path has one maintainer; that is the reason it must be droppable |
| An errand begun in one city and finished in the other | a mixed deployment; forbidden because of the session cache |
| Confidently wrong | a warm session holding another implementation's writes |
| The Tablets | `contracts/` — the shared JSON both implementations read |
| A spelling both cities agree on | `contracts/test-vectors`, `contracts/errors`, `contracts/migrations` |
| The Mint | `shared-native` and `gradido-blockchain-core` — determinism-critical code, one place only |
| Streets named, no houses | most directories are empty; the rebuild has barely started |
| Told that it is not built | `ROUTE_NOT_IMPLEMENTED` |
| Walked to the town hall instead | answering an unimplemented route with `index.html`; see `server/staticRoutes.ts` |
| Carving the answer on the way back | writing the legacy behavior into `contracts/test-vectors` once it is found |
| A desk | a SessionContext — one user's working set, held for the length of a session |
| The desk may burn | every session field must be safe to lose and reconstruct from the database |
| Setting out only what was asked for | lazy loading; never load the complete user state eagerly |
| Two pages of a ledger, not five hundred rows | the bounded working set — `DEFAULT_PAGINATION_PAGE_SIZE` 25, roughly two pages held |
| Cleared ten minutes after it was set out | `SESSION_HARD_TIMEOUT_MS`, counted from `session_created_at`, not from last use |
| A different clerk, whose desk is bare | a cold session on another instance behind the load balancer — acceptable, no sticky sessions |
| He changed it himself | own writes — the Interaction updates the session in place |
| The register of every desk holding a copy | global invalidation tracking; deliberately not built |
| The mark on the document | version / generation / sequence carried by the data |
| Noticing when he next reaches for his copy | foreign writes detected lazily on access, then refreshed |
| The ledger that only grows | append-only data, transactions above all |
| Four thousand seven hundred and eleven | a session cursor, caught up by loading only the missing range |
| Catching up | incremental synchronization, which replaces cache invalidation for append-only data |
| The board of questions in every office | AGENTS.md section 9, the checklist before caching anything |
| The wrong answer is to have one answer | one cache policy for every kind of data is wrong |
| Quarters laid out by trade | domain-centric structure — `domain/<name>/`, not global technical layers |
| A quarter standing empty | empty directories are intentional; the location does not move because it is unpopulated |
| The records | Data — `*.data.ts` |
| The small arithmetic | Data-Logic — `*.logic.ts`, e.g. `calculateDecay()`, `calculateBalance()` |
| The named business, one door | an Interaction — one file in `interactions/`, named after the operation |
| The clerk says when, the runner says how | the repository boundary: Interaction decides when, Repository decides how |
| The board in the square | the process-global caches on the AppContext — communities, settings, role rights, public contributions |
| No office more than ten minutes behind | global caches carry a 10-minute TTL and are invalidated at once on the instance that made the change |
| The fastest errand is the one nobody has to run | avoid work before optimizing the work |
| Eleven works in the right order | legacy's deployment — 11 compose services, two networks, nginx with four config files |
| Two books on how to raise it | `../gradido/deployment/bare_metal` and `deployment/hetzner_cloud` |
| A city must fit in a single file | the single-binary deployment: server, frontends and runtime in one executable |
| Someone who does not understand cities | the target founder — a community host who is not a server administrator |
| It digs its own well | SQLite, opened beside the binary when nothing is configured |
| The one question it cannot answer for itself | the setup conversation that establishes the home community |
| A chest beside it | avatars and uploads on the local filesystem behind the S3 interface, next to the binary |
| Faces kept inside the archive | legacy stores avatars as `mediumblob` rows; not carried over |
| A deep cistern instead of the well | PostgreSQL, for a founder who administers servers |
| A granary instead of the chest | Garage as the S3 backend — planned, https://garagehq.deuxfleurs.fr/ |
| Still the same file | the storage choice is configuration, not a different build or a different behavior |
| Senomagos writes a transfer down three times | legacy's `transactions`, `pending_transactions` and `dlt_transactions` |
| The thin fourth book | `dlt_transactions.verified` / `verified_at` / `error` — the reconciliation flag |
| The great register outside all the cities | the Hedera/Hiero topic the transactions are written to, read back by gradido-node |
| The scribe who reads both hands | legacy's `dlt-connector`, translating into blockchain format |
| Writing in the register's hand from the start | intended for gradido2: one transaction shape, signed once, no second format to reconcile |
| Making and confirming stop being two crafts | one path to create and confirm a transaction instead of two |
| The one question turned out to be eleven | the setup conversation now covers the community, the database and the mail relay |
| A city on a bench | a development installation — the proposal answered with one Enter |
| Holding the sheet up | the development block shown whole and taken or refused as one, not asked field by field |
| The box in the corner | the MailDev container of `docker-compose.yml`, port 1026, no TLS and no login |
| The sensible answer in the margin | every prompt's default, in parentheses; Enter takes it |
| The note nailed inside the gate | `.env` in the working directory, written by `setup` and read by both implementations |
| What the magistrate shouted stands | a variable already in the environment is never overridden by the file |
| Both cities read the same note | one dotenv form, `packages/service-core` and `service-core/src/env_file.c` |
| The letter-carrier | the SMTP relay named by the `EMAIL_*` variables |
| The word for the bag | `EMAIL_PASSWORD` — a credential, so env and never the settings table |
| The runner who hands over no letter | the startup probe: greeting, EHLO, the TLS upgrade, AUTH, and no message |
| The runner takes the carrier's own road | both paths probe through the same libcurl — `Mailer.verify()` and `sc_mail_session_probe` |
| One line on the board | `mail.relay.connected` at info, `mail.relay.failed` at warn — never fatal |
| A city that was never given a carrier | `EMAIL=false`, reported as `mail.relay.disabled` |
| The carters of Eporedon | the database workers of the C path — `fast-servers/service-core/include/service_core/db_exec.h` |
| A clerk standing at the Archive's window | an event loop blocked in a synchronous libpq call, stalling every connection on it |
| A clerk waits only at a door he can see from his desk | the Threading rule in `fast-servers/Architecture.md`: a wait with a file descriptor belongs on the event loop |
| A note with a hook for each answer | asynchronous libpq on the loop, written as callbacks — measured and not chosen, for the cost it puts on every repository |
| The exception written into the rule | PostgreSQL goes to workers instead of the loop — `Architecture.md`, *Threading* and *The executor* |
| A blink against a thousand | the measured hand-over, 0.1–7.5 µs a unit, against 25–200 µs a query and about a millisecond a commit |
| One cart and a bar across the door | the old `bc_context.db_lock` mutex around a single connection |
| Whichever clerk was free pushed whichever cart | the short-lived connection pool the loops took turns holding, replaced by the executor |
| A carter owns one cart for his whole life | each worker owns one connection for its whole life; its libpq buffers, TLS state and prepared statements stay on one core, and no lock guards it |
| Forms stamped once stay stamped | prepared statements, prepared once per connection — `service_core/sql.h` |
| As many carters as carts the Archive can load at once | `DB_POOL_SIZE`, sized from what the database server can do, never from the number of loops — `contracts/database-config.json`, `rules.pool` |
| A sleeping carter costs nothing but his bed | a worker blocked on its socket costs no core; only loops are one per core |
| The slate | the request's own arena, from `sc_http_alloc` — the unit of database work (`sc_db_unit`) lives on it; values in, rows out, nothing copied |
| Written from the top down, nothing rubbed out in between | arena allocation: a pointer bump per allocation, no piece freed on its own |
| Wiped once, all of it, when the answer has been read out | the arena goes back to the pool in one call after the handler returns, or after the resume callback of a parked request — the reply was already copied out by `sc_http_reply` |
| The clerk's own stack of slates | the per-loop graded arena pool in `service-core/src/http_arena.c`, thread-local and lock-free; wiped slates stay in stock for the next request |
| A slate comes back to the stack it was taken from | an arena is always freed on the loop that lent it, whichever worker wrote on it while it was parked |
| Only whoever holds the slate writes on it | the loop until it parks the request, the worker while it holds the unit (`sc_db_unit_alloc`), the loop again in `done` |
| A few sizes; the largest is the end of it | the grades 16 KiB, 64 KiB, 256 KiB and 1 MiB; a response that would not fit has to be paged |
| The board on the wall behind the clerk | the thread-local scratch arena a route parses its request body in (`backend/src/user_routes.c`), reset for every request and never parked |
| The bench | a parked request — `sc_http_defer` on the loop, `sc_http_resume` from the worker |
| Back to the clerk who wrote it | the unit returns to the loop that submitted it, where the request's arena is freed |
| Keep it, throw it away, once more with fresh numbers | `SC_DB_COMMIT`, `SC_DB_ROLLBACK`, `SC_DB_AGAIN` — transactions belong to the executor, never to a repository |
| A number that was somebody else's already | a generated value (gradido id, verification code) that collided with a unique index |
| A courtyard | a cache group: the CPUs one L3 serves; loops and workers are pinned per group — `service_core/topology.h` |
| Seats on the bench | `SC_DB_QUEUE_PER_WORKER`, the bounded queue per cache group |
| Come back in a moment | 503 `SERVICE_BUSY` with `Retry-After` — `contracts/errors/api.json`; nothing of the request has run |
| The sand in the glass | `SC_DB_QUEUE_WAIT_MS`, five seconds |
| The glass turned only when a carter came by | the first deadline check, made only when a worker took a unit: with the database stuck, queued requests waited as long as it did |
| The warden who only turns the glass | the sweeper thread in `db_exec.c`, answering expired units even while every worker is stuck |
| His own bucket | a SQLite read runs on the loop itself, on that loop's own read connection |
| The well has one rope | the single SQLite writer thread |
| The book that began again after its last page | `sqlite3_step` restarts a finished statement; the cursor in `sql_sqlite.c` now stays at its end |
| Sirodunon's bench, glass and sign | the `DatabaseGate` in `packages/backend-core/src/database/gate.ts`, answering the same 503 |
| One tireless man who leaves a note at the window | the TypeScript path's single event loop: PostgreSQL queries are asynchronous and do not block it; `bun:sqlite` is synchronous, so the well is the one place he waits himself |
