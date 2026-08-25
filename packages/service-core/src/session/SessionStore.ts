import * as v from 'valibot'
import type { Logger } from '..'
import {
  type SessionClaims,
  sessionStoreLimitsSchema,
  sessionTokenSchema,
  sessionUserUuidSchema,
} from './input.schema'

/**
 * The session store: sessions in slots the token names, expiry in creation order, and a slot
 * array that grows to whatever the load turns out to be.
 *
 * **Read `Architecture.md`, *Session cache*, and `fast-servers/Architecture.md`, *Session
 * cache*, before changing anything here.** This is that design with the concurrency taken
 * out, and the parts that look like they could be simplified away are the parts that were
 * argued for there.
 *
 * Two decisions carry everything else:
 *
 * **The token carries the slot, so a lookup is an array read.** There is no key to hash, so
 * there is no collision, no probe walk, and none of the silent hit-rate collapse that a cache
 * suffers when the keys it routes on can be chosen from outside. It also fixes what may
 * happen to a slot: **a live session is never moved**, because its index is out in the world,
 * inside tokens this process signed. That is why the store grows by appending and reuses
 * slots through a free list, rather than by rearranging anything.
 *
 * **A session holds every token it has issued, so a hit is a string comparison instead of a
 * signature check.** Measured here: 281 ns to read the claims through their schema and 6 ns
 * to find the session with them, against the 852 ns of HMAC that buys. So the store is looked
 * in *first*, with the claims read but not yet vouched for, and the signature is verified only
 * when nothing was found — on the path that then creates a session anyway. And that is the
 * smaller half of it: a miss does not merely verify, it builds a session, which is where the
 * database round trips are. `Architecture.md`, *Session cache*, has both halves.
 *
 * That second one puts this store on the authentication path, and it holds only because of
 * one rule, which every method here keeps:
 *
 * ```text
 * NOTHING an unverified token says is trusted as data. The claims select a candidate;
 * every decision is made against what the store itself holds.
 * ```
 *
 * A token is accepted because it is byte-identical to one this process minted and still
 * holds — that is what the signature would have proven, established by equality instead of
 * by arithmetic. Tokens in here are therefore credentials: they are never logged (the
 * redaction list in `contracts/logging.json` says so) and never handed to anything but a
 * comparison. And because a session is where they live, a session that is gone takes them
 * with it: after a restart, on another instance, or ten minutes on, the only way in is the
 * signature.
 *
 * The three structures, and why the obvious single one is not enough:
 *
 * ```text
 * slots   the sessions. Appended to, never reordered, never shrunk: an index that
 *         went out in a token has to keep meaning what it meant.
 * order   the slots in creation order. Expiry walks it from the front, which is
 *         what makes a timeout one comparison rather than a sweep. It cannot be
 *         derived from the slot order, because a reused slot is out of turn.
 * free    slots whose session is gone. Reuse is what keeps the store from growing
 *         once the load stops growing.
 * ```
 *
 * What the fast path in C needs and TypeScript does not:
 *
 * ```text
 * reference counting   the garbage collector already does exactly that: a session a
 *                      request still holds survives being dropped from the store
 * the store lock       every method here is synchronous, so no second request can
 *                      observe the store between two of their statements
 * ```
 */

export type SessionStoreConfig = {
  /**
   * The ceiling: how many slots the store may ever hold at once.
   *
   * It is **not** a sizing decision. How many sessions live at once is the number of them
   * created within one `hardTimeoutMs`, which depends on the community, the time of day and
   * what the clients are doing, and the store finds it by itself: it appends a slot when it
   * needs one and reuses the slots of sessions that have ended. Nothing is retired early
   * below this line.
   *
   * This is what keeps a load nobody planned for from ending the process instead of the
   * request. At the ceiling the oldest session is retired to make room, which its owner sees
   * as one miss and a verification, and `session.context.evicted` says it happened.
   *
   * **The number itself is still open**, and it is a memory question rather than a session
   * one: what a session costs in this runtime has to be measured before a machine's RAM can
   * be divided by it. `Architecture.md`, *Session cache*, records how — {@link size} and
   * {@link slotCount} beside the process's own resident memory, under a load that fills the
   * store. Until that has been done, pick a number that is obviously survivable rather than
   * one that looks precise.
   */
  maxSessions: number
  /** `SESSION_HARD_TIMEOUT_MS`. A session is dropped this long after it was created, whatever it is doing. */
  hardTimeoutMs: number
  /**
   * `JWT_TOKEN_REISSUE_AFTER_MS`. How old the newest token of a session has to be before
   * {@link SessionStore.refreshToken} mints another one.
   *
   * It lives here rather than at the caller because it is what bounds the token set: one
   * token per interval for as long as a session lives, so `hardTimeoutMs / this + 1` of
   * them. Deciding it from the token's own `iat` instead would hand that bound to whoever
   * writes the token.
   */
  tokenReissueAfterMs: number
  logger: Logger
  /** Injectable for tests. Wall clock, because `session_created_at` is compared against it. */
  now?: () => number
}

/** A new session and the token that reaches it. Both are made at once, or the session is unreachable. */
export type SessionCreation<T> = {
  readonly session: T
  readonly token: string
}

export type SessionHandle<T> = {
  readonly claims: SessionClaims
  readonly session: T
  readonly token: string
}

type SessionEntry<T> = {
  readonly userUuid: string
  readonly sessionCreatedAt: number
  /**
   * Every token minted for this session, and the reason a hit needs no signature check.
   * Bounded by the re-issue interval; an old token stays valid until the session dies,
   * because a request that was already in flight when a fresher one was issued still
   * carries it.
   */
  readonly tokens: Set<string>
  /** By this store's clock, never by a token's `iat`. */
  lastIssuedAt: number
  readonly session: T
}

/** How long `order` may drag its emptied front around before it is compacted. */
const ORDER_COMPACT_THRESHOLD = 64

export class SessionStore<T> {
  public readonly maxSessions: number
  public readonly hardTimeoutMs: number
  public readonly tokenReissueAfterMs: number

  private readonly logger: Logger
  private readonly now: () => number

  /** Sessions by slot. Appended to and written in place; a live session never moves. */
  private readonly slots: (SessionEntry<T> | undefined)[] = []
  /** Slots in creation order. Everything before {@link orderHead} is already reclaimed. */
  private readonly order: number[] = []
  private orderHead = 0
  /** Slots that hold nothing, newest first. Empty means the next session appends a slot. */
  private readonly free: number[] = []

  private liveCount = 0
  /** Keeps `sessionCreatedAt` non-decreasing, so `order` stays ordered even if the clock steps back. */
  private lastCreatedAt = 0

  public constructor(config: SessionStoreConfig) {
    /* Throws a ValiError naming the field and what was wrong with it. A store built with a
       ceiling of zero would answer every request with a miss and never say why, so this is
       the one failure here that must happen at construction rather than under load. */
    const limits = v.parse(sessionStoreLimitsSchema, config)
    this.maxSessions = limits.maxSessions
    this.hardTimeoutMs = limits.hardTimeoutMs
    this.tokenReissueAfterMs = limits.tokenReissueAfterMs
    this.logger = config.logger
    this.now = config.now ?? Date.now
  }

  /** How many sessions are alive right now. */
  public get size(): number {
    return this.liveCount
  }

  /**
   * How many slots the store has ever needed at once — the high-water mark, since slots are
   * reused but never given back.
   *
   * This and {@link size} are what a status route reports and what the experiment behind
   * {@link SessionStoreConfig.maxSessions} measures against the process's resident memory.
   */
  public get slotCount(): number {
    return this.slots.length
  }

  /**
   * The read path: the claims of a token nobody has verified, and the token itself.
   *
   * A miss is not an error and not a rejection. It says only that this token cannot be
   * answered from memory, so the caller verifies its signature — and creates a session with
   * {@link create} if it turns out to be genuine. A cold instance, a restart, an expired
   * session and a forged token all arrive here as the same `undefined`.
   */
  public get(claims: SessionClaims, token: string): T | undefined {
    return this.find(claims, token)?.session
  }

  /**
   * The token the client should carry from here on, or undefined when the one it sent is
   * still current.
   *
   * Calling this on every authenticated request is the intended use: it mints only when the
   * newest token of the session is older than the re-issue interval, which is what keeps the
   * token set small and the login sliding in whole intervals rather than continuously.
   *
   * `mint` is handed the store's own claims, not the ones that arrived. A claim that was
   * never verified must not end up inside something this process signs.
   */
  public refreshToken(
    claims: SessionClaims,
    token: string,
    mint: (claims: SessionClaims) => string,
  ): string | undefined {
    const entry = this.find(claims, token)
    if (entry === undefined) {
      return undefined
    }
    const now = this.now()
    if (now - entry.lastIssuedAt < this.tokenReissueAfterMs) {
      return undefined
    }
    const fresh = v.parse(
      sessionTokenSchema,
      mint({
        slot: claims.slot,
        userUuid: entry.userUuid,
        sessionCreatedAt: entry.sessionCreatedAt,
      }),
    )
    entry.tokens.add(fresh)
    entry.lastIssuedAt = now
    return fresh
  }

  /**
   * Creates a session, and it never fails for want of space.
   *
   * Whoever creates a session pays for expiry: everything at the front of `order` that has
   * timed out is released first — a burst of them at once, not one per insertion — and their
   * slots are what the new session is usually written into. Only when none came free does the
   * store append a slot, which is how it arrives at the size the load actually has.
   *
   * At the ceiling, and only there, the oldest live session is retired to make room. It
   * survives for whoever is currently working with it, because holding it is what keeps it
   * alive, and its owner's next request degrades to a miss rather than to an error.
   *
   * `build` receives the claims and returns the session **and** the token minted from them:
   * one call, because a session whose token was never registered is a session nothing can
   * ever reach again. It runs before the slot is committed, so a session that throws while
   * being built leaves nothing behind.
   */
  public create(
    userUuid: string,
    build: (claims: SessionClaims) => SessionCreation<T>,
  ): SessionHandle<T> {
    const now = this.now()
    this.reclaimExpired(now)
    if (this.free.length === 0 && this.slots.length >= this.maxSessions) {
      this.retireOldest(now)
    }

    /* Never below the last one: expiry walks `order` from the front and stops at the first
       session that is still live, so a clock that steps backwards would strand everything
       behind it. */
    const owner = v.parse(sessionUserUuidSchema, userUuid)
    const sessionCreatedAt = Math.max(now, this.lastCreatedAt)
    /* Decided before `build` runs and committed after it, so a throw costs nothing. */
    const slot = this.free.at(-1) ?? this.slots.length
    const claims: SessionClaims = { slot, userUuid: owner, sessionCreatedAt }
    const built = build(claims)
    const token = v.parse(sessionTokenSchema, built.token)

    const entry: SessionEntry<T> = {
      userUuid: owner,
      sessionCreatedAt,
      tokens: new Set([token]),
      lastIssuedAt: now,
      session: built.session,
    }
    if (slot === this.slots.length) {
      this.slots.push(entry)
    } else {
      this.free.pop()
      this.slots[slot] = entry
    }
    this.order.push(slot)
    this.liveCount++
    this.lastCreatedAt = sessionCreatedAt
    this.logger.debug(
      { cat: 'session', event: 'session.context.created' },
      'session context created',
    )
    return { claims, session: built.session, token }
  }

  /**
   * Ends one session before its timeout — logout, or a change that must not be allowed to
   * linger in a working set. Every token of that session dies with it.
   *
   * It asks for the token for the same reason {@link get} does: without the signature, a
   * bare set of claims is a wish, and ending someone else's session on one would be an
   * invitation. Whoever may end a session has just been authenticated with its token.
   *
   * The slot is emptied but not handed back yet — it is still standing in `order`, and
   * freeing it here would put the same index in `free` twice once expiry reaches it. It
   * becomes available when its turn comes, which costs one slot for the rest of a timeout
   * window and saves the store from having to search for its own bugs.
   */
  public invalidate(claims: SessionClaims, token: string): boolean {
    if (this.find(claims, token) === undefined) {
      return false
    }
    this.slots[claims.slot] = undefined
    this.liveCount--
    return true
  }

  /**
   * The whole read path, in the order the design requires, and the only place that decides
   * whether a token names a session.
   *
   * ```text
   * the claim's age    a token whose session would be long dead never touches the store
   * the slot           range-checked, because nothing here has been vouched for
   * the entry's age    the authoritative one: the claim above is only a filter
   * the user           a cheap comparison before the expensive one
   * the token          byte-identical to one this session was given, or nothing
   * ```
   *
   * The claim's age is checked first because it is free and keeps a dead token off the store
   * entirely. It is not what makes the timeout hold: a claim can say anything until a
   * signature has been checked, so the entry's own creation time is compared as well. The
   * two differ in exactly one case — a token whose session has timed out but whose slot
   * expiry has not reached yet — and that case is the reason the second check exists.
   *
   * The slot is checked twice over, and both are wanted. `sessionClaimsSchema` in
   * `input.schema.ts` is where an absent or malformed claim becomes a miss rather than slot 0, which is a valid
   * slot and usually holds someone. The line below is what stands between the store and a
   * caller who handed it the output of `JSON.parse` cast to the type — `any` defeats every
   * guarantee the compiler could give — and it is worth having because it costs 6 ns where
   * parsing again would cost 281. An index beyond the slots that exist can only be checked
   * here, since it depends on how far the store has grown.
   *
   * The `user_uuid` comparison covers the one case where a live token can address a slot
   * that now belongs to someone else, the store having reused it. The token comparison would
   * catch that too; it comes second because 36 characters are cheaper to reject than a hash
   * of the whole token.
   */
  private find(claims: SessionClaims, token: string): SessionEntry<T> | undefined {
    const now = this.now()
    if (!this.isWithinTimeout(claims.sessionCreatedAt, now)) {
      return undefined
    }
    if (!Number.isInteger(claims.slot) || claims.slot < 0 || claims.slot >= this.slots.length) {
      return undefined
    }
    const entry = this.slots[claims.slot]
    if (entry === undefined) {
      return undefined
    }
    if (!this.isWithinTimeout(entry.sessionCreatedAt, now)) {
      return undefined
    }
    if (entry.userUuid !== claims.userUuid) {
      return undefined
    }
    if (!entry.tokens.has(token)) {
      return undefined
    }
    return entry
  }

  private isWithinTimeout(createdAt: number, now: number): boolean {
    return now - createdAt < this.hardTimeoutMs
  }

  /**
   * Walks `order` from the front for as long as it finds timed-out sessions and slots
   * {@link invalidate} emptied, so a burst of expiries is released in one go. It stops at the
   * oldest session that is still live, and nothing else moves the front.
   *
   * This is the whole of expiry: no sweeper, no timer, and nothing on the read path. It is
   * paid by whoever creates the next session, which is also the only moment its result is
   * needed.
   */
  private reclaimExpired(now: number): void {
    while (this.orderHead < this.order.length) {
      const slot = this.order[this.orderHead]
      const entry = this.slots[slot]
      if (entry !== undefined && this.isWithinTimeout(entry.sessionCreatedAt, now)) {
        break
      }
      this.orderHead++
      this.slots[slot] = undefined
      this.free.push(slot)
      if (entry !== undefined) {
        this.liveCount--
        this.logger.debug(
          {
            cat: 'session',
            event: 'session.context.expired',
            data: { ageMs: now - entry.sessionCreatedAt },
          },
          'session context dropped after its hard timeout',
        )
      }
    }
    this.compactOrder()
  }

  /**
   * Drops the oldest entry of `order` to free one slot, live or not.
   *
   * Only reachable at the ceiling, with nothing expired and nothing free, so this line means
   * the store was not allowed to grow to the load it actually has. That is a sizing decision
   * meeting reality, not a fault, and it is logged rather than counted silently.
   */
  private retireOldest(now: number): void {
    if (this.orderHead >= this.order.length) {
      return
    }
    const slot = this.order[this.orderHead]
    const entry = this.slots[slot]
    this.orderHead++
    this.slots[slot] = undefined
    this.free.push(slot)
    if (entry !== undefined) {
      this.liveCount--
      this.logger.warn(
        {
          cat: 'session',
          event: 'session.context.evicted',
          data: { ageMs: now - entry.sessionCreatedAt, capacity: this.maxSessions },
        },
        'session store is at its ceiling, retiring the oldest session before its timeout',
      )
    }
    this.compactOrder()
  }

  /**
   * Throws away the reclaimed front of `order` once it is worth the copy. Without this the
   * array would grow by one entry per session for as long as the process runs; with it, both
   * the copy and the memory are proportional to what is actually live.
   */
  private compactOrder(): void {
    if (this.orderHead === 0) {
      return
    }
    if (this.orderHead === this.order.length) {
      this.order.length = 0
      this.orderHead = 0
      return
    }
    if (this.orderHead >= ORDER_COMPACT_THRESHOLD && this.orderHead * 2 >= this.order.length) {
      this.order.splice(0, this.orderHead)
      this.orderHead = 0
    }
  }
}
