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
