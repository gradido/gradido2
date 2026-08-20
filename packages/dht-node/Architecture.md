# packages/dht-node Architecture

Peer discovery on the reference path: a `js-libp2p` node.

**Read `../../Architecture.md`, *Peer discovery*, first.** It holds why this component is the
one mirrored thing with no shared code, and what replaces the shared code. This file holds
only what is specific to the TypeScript half.

Its counterpart is [`fast-servers/dht-node`](../../fast-servers/dht-node/Architecture.md),
which is `rust-libp2p` behind an `extern "C"` header. The two are kept together by the libp2p
specification and by an interop test — not by a contract vector, and not by anyone
remembering to change both.

---

## What this package is not

```text
not a database client   it discovers peers and reports them, nothing else
not a domain component  federation rows are written by an Interaction through a
                        Repository in backend-core
```

Legacy's `dht-node` writes `federated_communities` rows itself. That is the one behavior
deliberately not carried over — see the fast-path file for why, the reasoning is the same on
both sides.

---

## What must match the other half

A libp2p peer id is derived from the node's key pair, and Gradido derives that key pair from
`FEDERATION_DHT_SEED` — the same key that ends up in `communities.public_key` and that the
federation handshake identifies a community by.

If the two nodes derive different peer ids from the same seed, one community appears on the
network as two peers depending on which path is running, and nothing fails — the network
simply grows a duplicate. So both derivations are contracted as test vectors:

```text
seed (32 bytes)  -> ed25519 key pair  -> libp2p peer id
topic string     -> the DHT key announced and looked up under
```

They are the one part of peer discovery a contract vector can cover, which is exactly why
they are covered.

---

## Shape

The node runs in its own process, as it does in legacy, and reports what changed rather than
what it knows: the sweep is O(communities) every 20 seconds, so a full snapshot across a
process boundary every cycle is work nobody asked for. A consumer that wants the full picture
reads the database, which is where the picture belongs.

`js-libp2p` is pinned. It is not raised without the interop test green against the pinned
`rust-libp2p` — two libraries with their own release cycles can diverge on a protocol detail
without either being wrong. The fast path runs its own node rather than reading what this one
writes, so the two really are on the network at the same time and the interop test is checking
the deployed arrangement rather than a hypothetical one.

---

## Bootstrap

A fresh community is given **one Gradido community URL** and nothing else. It asks that
community over plain HTTP for a peer list, dials what comes back, and is in the DHT. Every
running community can answer, because every running community already serves HTTP — so there
is no dedicated bootstrap infrastructure to keep alive for a decade.

The default is `gdd.gradido.net`, which the Gradido project operates. An operator who
configures nothing bootstraps from us; an operator who configures something else bootstraps
from any other running community, and both work the same way.

The route is served by the **backend**, not by this package — it is contracted in
[`contracts/server/backend/peer.json`](../../contracts/server/backend/peer.json) as
`peer.bootstrap`. The backend is where it belongs for the same reason everything else here
does: this package discovers peers and reports them, and something else decides what to do
with that.

What goes into the answer is a *sample* of peers that are current in the DHT sense — a few
different entry points rather than the full set, and not the same few every time. Kademlia
fans out from wherever it starts, so several peers spread across the keyspace seed a usable
routing table where five neighbours of one another seed a corner of it.

Two properties of the route are load-bearing. It is **public**, because bootstrapping happens
before any handshake exists. And what it returns is a **hint, not a trust decision**: a peer
is verified when it is contacted, not when it is named, so a poisoned list costs time rather
than trust. Signing the answer — handshake with the bootstrap community first — is planned
hardening on top of that layering, not a replacement for it.

Transports are **TCP + QUIC, with circuit relay v2 as the fallback**; WebRTC is not enabled,
because Gradido's peers are servers and the frontend never joins the DHT. The reasoning is in
the fast-path file and applies to both halves.

One thing to check before pinning: QUIC in `js-libp2p` is younger than in `rust-libp2p`. If it
does not work here, the pair is TCP-only in practice and every community behind a home router
loses reachability — the interop test has to cover the QUIC path specifically, not just "they
found each other".

---

## Open

Both remaining questions are shared with the fast path and are recorded in
[`fast-servers/dht-node/Architecture.md`](../../fast-servers/dht-node/Architecture.md): what
the signed bootstrap answer looks like on the wire, and how recent "seen recently" has to be.
Answering either one separately is the failure mode.
