# fast-servers/dht-node Architecture

Peer discovery on the fast path: a `rust-libp2p` node, built as a static library behind an
`extern "C"` header and linked by the C server like any other native module.

**Read `../../Architecture.md`, *Peer discovery*, first.** It holds why this component is
mirrored instead of shared, and what replaces the shared code. This file holds only the
boundary.

---

## Why Rust, and why only here

Every other native module in this repository exists because a computation must produce the
same result everywhere, or because a wire format must not be written twice. This one exists
because the library does not exist in C.

What federation needs is not a Kademlia routing table on its own. It is the routing table
plus the transports, the multiplexing, the NAT traversal, the peer store and the identify
handshake that make a node reachable from behind a home router — which is where a Gradido
community server is expected to run. `rust-libp2p` and `js-libp2p` are the two places that
system exists. Writing a third is not a module, it is a second project.

**The fast path runs its own node.** That decision is what puts Rust in this repository at
all: if the fast server could read rows a TypeScript process writes, this module would not
exist and neither would the third toolchain.

It cannot, for the two cases the fast path is *for*. On the small server, a second Node
runtime beside the C one is a whole process's worth of RAM spent on peer discovery — the
figure in `../../Architecture.md` that decides whether the thing runs beside its database at
all. On the high-load server, a fast path that needs a TypeScript process to reach the network
is not droppable, it is merely rearranged; the rule exists so that running without the other
path requires no code change anywhere.

Rust is therefore a leaf language here in exactly the sense C++ already is: one module, one
`extern "C"` surface, no application logic behind it. `../AGENTS.md`, section 2, holds the
rules; they are the C++ ones with panics in place of exceptions.

**Do not add a second Rust module.** A third toolchain on the fast path is a design
decision, and it is recorded in `../../Architecture.md` rather than in a `Cargo.toml`.

---

## What this module is not

```text
not a database client   it discovers peers and reports them, nothing else
not a domain component  federation rows are written by an Interaction through a
                        Repository, on whichever path is running
not a config parser     the caller passes what it already read
not a scheduler         the caller decides when to drain, the node decides when
                        to announce
```

Legacy's `dht-node` writes `federated_communities` rows itself. That is the one behavior
deliberately not carried over: a network library in the persistence layer puts persistence
decisions where the rest of the architecture does not look for them.

---

## The identity problem

This is the part that can silently break federation, so it comes before the API.

A libp2p peer id is derived from the node's key pair. Legacy derives that key pair from
`FEDERATION_DHT_SEED` and then stores it as the home community's `public_key` and
`private_key` — the same key `communities.public_key` carries and the federation handshake
identifies a community by.

Both nodes must therefore derive **the same peer id from the same seed**. If they do not, one
community appears on the network as two peers depending on which path is running, every other
community stores both, and nothing fails: the network simply grows a duplicate that half the
participants trust and half do not.

Two derivations have to agree, and neither is expressible as a route or a table:

```text
seed (32 bytes)  -> ed25519 key pair  -> libp2p peer id
topic string     -> the DHT key announced and looked up under
```

Both belong in `contracts/test-vectors` as input/expected pairs — a seed and its peer id, a
topic and its key. They are the one part of peer discovery a contract vector *can* cover, and
they are worth covering precisely because everything around them cannot be.

---

## The boundary

Four calls. The shape matters more than the names:

```text
init      (seed, topic, listen addresses, bootstrap peers) -> handle
                 starts the node on its own runtime thread
drain     (handle, caller buffer)                          -> count
                 copies out what changed since the last drain
announce  (handle)                                         -> error code
                 publish the local record now, out of band with the timer
shutdown  (handle)                                         -> error code
                 stops the runtime and frees everything the handle owns
```

**Threading.** libp2p runs a tokio runtime; h2o runs an event loop. Neither may block the
other, so the node owns a thread and the two speak through the drain. `drain` never waits for
the network — it returns what has already arrived, or zero.

**Ownership.** The module owns the peer state; the caller owns the buffer it drains into. A
drained record is a value copy, not a borrow: nothing the caller holds points into the
module after the call returns. That is what makes the caller's arena discipline hold across
the boundary — see `../AGENTS.md`, section 1.

**Nothing crosses but bytes and lengths.** No Rust type, no `String`, no allocation the
caller has to free. A peer is a fixed-size record: peer id, api version, endpoint, last-seen
timestamp, and a flag saying whether it appeared, changed or went away.

**Panics stop at the boundary.** Every exported function catches, logs and returns an error
code. A panic unwinding into h2o's event loop is undefined behavior for the same reason a C++
exception is.

**Changes, not snapshots.** The sweep is O(communities) every 20 seconds, which at a few
thousand communities is real work rather than a poll. The node keeps the state and reports
only what moved, so the boundary scales with the number of changes rather than the size of
the network. A caller that wants a full picture asks the database, which is where the picture
belongs.

---

## Safety

Safe Rust ends at the `extern "C"` line, so the module is not exempt from anything in
`../Architecture.md`, *Safety net*:

- `#![forbid(unsafe_code)]` in the interior. The `unsafe` lives in one file — the one that
  turns caller pointers into slices — and that file is small enough to review in one sitting.
- That file is fuzzed like a parser, because what reaches it is a peer list built from what
  strangers on the network said.
- The sanitizers run over the linked binary. Rust's guarantees say nothing about the pointer
  and the length the C side passed in.

---

## Versions

Two libraries with their own release cycles can diverge on a protocol detail without either
being wrong. So both are pinned — `rust-libp2p` here, `js-libp2p` in `packages/dht-node` —
and neither is raised without the interop test green on the pair.

The interop test is the merge gate for this component: start both nodes, have each discover
the other and a third peer, fail the build if either cannot. It lives in CI rather than in
`contracts/test-vectors`, because there is no value to compare — only a behavior between two
running processes.

---

## Transports

libp2p does not have "a" connection; it has a set of transports, and which ones are enabled
decides who can reach whom. Three matter here, and they are not alternatives to each other.

**TCP is the floor.** It works whenever the node is actually reachable — a public IP, or a
forwarded port. Behind a home router without port forwarding, inbound TCP does not arrive,
and TCP hole punching needs a simultaneous open that routers frequently refuse.

**QUIC is what makes a node behind that router reachable.** It runs over UDP with TLS 1.3
inside the protocol, and it buys three things:

```text
one round trip to an encrypted, multiplexed connection
    TCP needs the handshake, then a security negotiation (Noise or TLS), then a
    stream muxer (yamux). Against a few thousand peers every 20 seconds, the
    difference is the sweep, not a microbenchmark.
multiplexing without a muxer layer
    streams are in the protocol, so one slow stream does not block the others
UDP hole punching that works
    a UDP binding is far easier to punch than a TCP simultaneous open, which is
    what makes libp2p's DCUtR practical rather than theoretical
```

Its cost is that some networks throttle or block UDP, which is why TCP stays and why a relay
fallback exists rather than being optional.

**WebRTC is the browser transport, and Gradido has no browser peers.** Its two libp2p forms
answer questions we do not have: `webrtc-direct` lets a browser dial a public node without a
signalling server, and private-to-private `webrtc` lets two NATed peers meet through a
signalling relay and then connect over ICE. Gradido's peers are community servers, and the
frontend talks to its own backend over HTTP — it never joins the DHT. The NAT-to-NAT case
WebRTC would cover is covered by QUIC plus DCUtR, with a relay when hole punching fails.

So the set is **TCP + QUIC, with circuit relay v2 as the fallback**, and WebRTC is not
enabled. Adding it later is a decision about letting browsers be peers, and should be argued
as that rather than as a transport detail.

The one thing to verify before pinning: QUIC in `js-libp2p` is considerably younger than in
`rust-libp2p`, which has had it since quinn. Both halves must have it working, or the
transport is TCP-only in practice and the NAT case comes back — this is exactly what the
interop test is for.

---

## Bootstrap

A Kademlia node needs somewhere to start, and libp2p — unlike legacy's hyperswarm — comes
with no public network to join.

**The answer is a public route on the backend that hands out peers.** A fresh community is
configured with one Gradido community URL, asks it over plain HTTP for a peer list, dials
those peers and is in the DHT. Nothing else is needed: no dedicated bootstrap infrastructure,
no hardcoded node addresses to keep alive for a decade, no mDNS that only works on a LAN.
Every community that is already running is a bootstrap node, because every community already
serves HTTP.

The route is contracted in
[`contracts/server/backend/peer.json`](../../contracts/server/backend/peer.json). Two
properties of it are load-bearing:

- **It is public.** Bootstrapping happens before any handshake exists — a route that needed
  authentication could not be the way in.
- **What it returns is a hint, not a trust decision.** Whoever answers could hand out a
  poisoned list. The peers it names are somewhere to start dialing; who a community *is* is
  settled by the federation handshake against `communities.public_key`, exactly as before.
  An implementation that treats a bootstrap answer as authority has moved the trust boundary
  to whoever the operator typed into a config file.

The answer always contains the answering community itself, so one URL yields at least one
dialable peer even from a community that knows nobody yet.

### What gets handed out

A handful of peers that are **current in the DHT sense** — seen recently, believed reachable
now. Not the full set, and not the `federated_communities` table: a bootstrap answer full of
nodes that announced once in 2023 is a slow failure rather than a fast one.

Nor the same handful every time. What a joining node needs is a few *different* entry points,
because Kademlia fans out from wherever it starts: several peers spread across the keyspace
seed a usable routing table, while five neighbours of one another seed a corner of it. So the
selection samples rather than takes the top of a list, and two calls a minute apart may
legitimately return different peers.

That also keeps the route from being an enumeration endpoint by construction rather than by
promise — nobody can walk the network by calling it repeatedly.

### The poisoned list, and what actually helps

Whoever answers could hand out peers that lead nowhere or somewhere hostile. Three things
stand between that and a problem, in the order they take effect:

**The default is `gdd.gradido.net`.** For as long as we run it, the ordinary case is a
community bootstrapping from us, and the attack needs an operator to have configured someone
else's URL. That is not a protocol guarantee and should not be described as one — it is the
common case being safe, which is worth having and worth not confusing with the other two.

**A peer is verified when it is contacted, not when it is named.** The bootstrap answer gets a
node dialing; each peer it dials then proves itself the same way it always did, through the
federation handshake against `communities.public_key`. A list full of impostors costs time,
not trust. This layering is the actual defence and it must not be collapsed: an implementation
that writes a bootstrap answer into `federated_communities` unverified has moved the trust
boundary into a config file.

**The list can be signed.** Running the federation handshake with the bootstrap community
*first*, and having it sign what it returns, turns an anonymous hint into an attributable
statement: it removes the impostor at a hijacked URL, and it makes a bad list traceable to a
real community rather than to nobody. It does not make the named peers trustworthy — that
stays the job of the handshake with each of them.

The cost is the chicken and egg: verifying a signature needs the signer's public key, so a
bootstrap entry becomes a URL *and* a key rather than a URL. For `gdd.gradido.net` that key
ships as a default and costs an operator nothing; for any other bootstrap it is one more thing
to configure correctly. That is why signing is worth building and worth building *second* —
the unsigned route is what lets a community join at all, and hardening it must not become the
reason it cannot.

---

## Open

- **When the signed variant lands, and what it looks like on the wire.** The route contract
  has no signature field yet, deliberately: adding one before the handshake-first flow is
  designed would fix a shape nobody has tried. See
  [`contracts/server/backend/peer.json`](../../contracts/server/backend/peer.json).
- **What "seen recently" means in numbers.** The sampling above needs a freshness bound, and
  both implementations must use the same one or the two hand out different notions of current.
  It belongs in `contracts/const.json` once the node exists to measure it against.
