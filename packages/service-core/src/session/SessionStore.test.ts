import { describe, expect, test } from 'bun:test'
import * as v from 'valibot'
import { Logger } from '../logging'
import type { SessionClaims } from './input.schema'
import { SessionStore } from './SessionStore'

const quietLogger = Logger.create({ LOG_LEVEL: 'fatal', LOG_FILE: '', NODE_ENV: 'test' })

const HARD_TIMEOUT_MS = 600_000
const REISSUE_AFTER_MS = 60_000

/** A session is whatever the caller builds; here it is its claims and a name to recognise it by. */
type TestSession = { claims: SessionClaims; name: string }

/**
 * users.gradido_id is a uuid and the schema rejects anything else, so the tests name their
 * users the way the database does. The label is what the assertions read.
 */
const uuidOf = (label: string) => `8b9a1e2c-0000-4000-8000-${label.padStart(12, '0')}`

/**
 * A store on a clock the test moves, with a stand-in for the signing: a minted token is a
 * string nobody can produce without asking, which is all the real one is on this path.
 */
const storeOn = (
  maxSessions: number,
  { hardTimeoutMs = HARD_TIMEOUT_MS, tokenReissueAfterMs = REISSUE_AFTER_MS } = {},
) => {
  let now = 1_700_000_000_000
  let minted = 0
  const store = new SessionStore<TestSession>({
    maxSessions,
    hardTimeoutMs,
    tokenReissueAfterMs,
    logger: quietLogger,
    now: () => now,
  })
  const mint = (claims: SessionClaims) =>
    `signed.${claims.slot}.${claims.userUuid}.${claims.sessionCreatedAt}.${++minted}`
  return {
    store,
    mint,
    at: () => now,
    advance: (ms: number) => {
      now += ms
    },
    setNow: (value: number) => {
      now = value
    },
    create: (label: string) =>
      store.create(uuidOf(label), (claims) => ({
        session: { claims, name: `session-${label}` },
        token: mint(claims),
      })),
  }
}

describe('SessionStore.get', () => {
  test('finds the session its token names', () => {
    const { store, create } = storeOn(4)
    const { claims, token, session } = create('a')
    expect(store.get(claims, token)).toBe(session)
  })

  // The whole point of looking before verifying: acceptance rests on the token being one
  // this store handed out, not on what its claims say. A token nobody minted here is a
  // miss, and the caller then pays for a signature check rather than getting an answer.
  test('claims that fit a session do not open it without its token', () => {
    const { store, create } = storeOn(4)
    const { claims } = create('a')
    /* Every claim of it fits, down to the counter being one off. */
    const almost = `signed.${claims.slot}.${claims.userUuid}.${claims.sessionCreatedAt}.2`
    expect(store.get(claims, almost)).toBeUndefined()
    expect(store.get(claims, '')).toBeUndefined()
  })

  test('another user cannot read the slot', () => {
    const { store, create } = storeOn(4)
    const { claims, token } = create('a')
    expect(store.get({ ...claims, userUuid: uuidOf('b') }, token)).toBeUndefined()
  })

  // The bound is the slots that exist, not the ceiling: the store grows into that room and
  // an index it has not reached yet names nothing. Everything else about a slot -- whole,
  // not negative, present at all -- is the schema's, and is tested there.
  test('a slot the store has not handed out is a miss, not an exception', () => {
    const { store, create } = storeOn(4)
    const { claims, token } = create('a')
    for (const slot of [1, 4, 4096]) {
      expect(store.get({ ...claims, slot }, token)).toBeUndefined()
    }
  })

  test('a slot whose session has ended is a miss', () => {
    const { store, create } = storeOn(4)
    const first = create('a')
    const second = create('b')
    store.invalidate(second.claims, second.token)

    expect(second.claims.slot).toBe(1)
    expect(store.get({ ...first.claims, slot: 1 }, first.token)).toBeUndefined()
  })

  test('a claim past the hard timeout is a miss while the ring is still untouched', () => {
    const { store, create, advance } = storeOn(4)
    const { claims, token } = create('a')
    advance(HARD_TIMEOUT_MS)

    expect(store.get(claims, token)).toBeUndefined()
    /* Nothing swept it: expiry is paid by the next creation, not by this lookup. */
    expect(store.size).toBe(1)
  })

  // The claim is only a filter. Nothing has verified it, so a token can claim to belong to
  // a session younger than it is -- and the entry's own creation time, which this store
  // wrote itself, is what actually ends the session.
  test('a forged creation time does not keep a timed-out session alive', () => {
    const { store, create, advance, at } = storeOn(4)
    const { claims, token } = create('a')
    advance(HARD_TIMEOUT_MS)

    expect(store.size).toBe(1)
    expect(store.get({ ...claims, sessionCreatedAt: at() }, token)).toBeUndefined()
  })

  test('the last millisecond of the window still hits', () => {
    const { store, create, advance } = storeOn(4)
    const { claims, token, session } = create('a')
    advance(HARD_TIMEOUT_MS - 1)
    expect(store.get(claims, token)).toBe(session)
  })

  test('a token whose slot was handed on finds nothing', () => {
    const { store, create } = storeOn(1)
    const mine = create('a')
    /* One slot, so the second session takes the first one's place. */
    const theirs = create('b')

    expect(mine.claims.slot).toBe(theirs.claims.slot)
    expect(store.get(mine.claims, mine.token)).toBeUndefined()
    expect(store.get(theirs.claims, theirs.token)).toBe(theirs.session)
  })
})

describe('SessionStore.refreshToken', () => {
  test('keeps the current token while it is younger than the interval', () => {
    const { store, create, mint, advance } = storeOn(4)
    const { claims, token } = create('a')
    advance(REISSUE_AFTER_MS - 1)

    expect(store.refreshToken(claims, token, mint)).toBeUndefined()
  })

  test('mints once the newest token is older than the interval, and the old one stays valid', () => {
    const { store, create, mint, advance } = storeOn(4)
    const { claims, token, session } = create('a')
    advance(REISSUE_AFTER_MS)

    const fresh = store.refreshToken(claims, token, mint)
    expect(fresh).toBeString()
    expect(store.get(claims, fresh as string)).toBe(session)
    /* A request that was already in flight still carries the old one. */
    expect(store.get(claims, token)).toBe(session)
  })

  // What bounds the token set: one token per interval, however often this is called. The
  // decision is made from this store's clock, not from the token's own iat -- otherwise
  // whoever writes the token decides how much memory the session takes.
  test('mints once per interval however often it is asked', () => {
    const { store, create, mint, advance } = storeOn(4)
    const { claims, token } = create('a')
    advance(REISSUE_AFTER_MS)

    expect(store.refreshToken(claims, token, mint)).toBeString()
    expect(store.refreshToken(claims, token, mint)).toBeUndefined()
    advance(REISSUE_AFTER_MS)
    expect(store.refreshToken(claims, token, mint)).toBeString()
  })

  test('signs the store´s claims rather than the ones that arrived', () => {
    const { store, create, advance, at } = storeOn(4)
    const { claims, token } = create('a')
    advance(REISSUE_AFTER_MS)

    const seen: SessionClaims[] = []
    store.refreshToken({ ...claims, sessionCreatedAt: at() }, token, (given) => {
      seen.push(given)
      return 'signed.fresh'
    })

    expect(seen).toHaveLength(1)
    expect(seen[0].sessionCreatedAt).toBe(claims.sessionCreatedAt)
    expect(seen[0].userUuid).toBe(uuidOf('a'))
  })

  test('refuses to refresh what it cannot find', () => {
    const { store, create, mint, advance } = storeOn(4)
    const { claims } = create('a')
    advance(REISSUE_AFTER_MS)

    expect(store.refreshToken(claims, 'signed.somewhere.else', mint)).toBeUndefined()
  })

  test('refuses a token that would reach nothing', () => {
    const { store, create, advance } = storeOn(4)
    const { claims, token } = create('a')
    advance(REISSUE_AFTER_MS)

    expect(() => store.refreshToken(claims, token, () => '')).toThrow(v.ValiError)
  })
})

describe('SessionStore.create', () => {
  test('hands the session its own claims and returns the token minted from them', () => {
    const { store, create } = storeOn(4)
    const { claims, session, token } = create('a')

    expect(session.claims).toEqual(claims)
    expect(claims.userUuid).toBe(uuidOf('a'))
    expect(store.get(claims, token)).toBe(session)
  })

  test('refuses a session nothing could ever reach', () => {
    const { store } = storeOn(4)
    expect(() =>
      store.create(uuidOf('a'), (claims) => ({ session: { claims, name: 'x' }, token: '' })),
    ).toThrow(v.ValiError)
    expect(store.size).toBe(0)
  })

  test('a session that throws while being built leaves no slot behind', () => {
    const { store, create } = storeOn(2)
    expect(() =>
      store.create(uuidOf('a'), () => {
        throw new Error('the user was deleted between the token and the load')
      }),
    ).toThrow('the user was deleted')

    expect(store.size).toBe(0)
    expect(create('b').claims.slot).toBe(0)
  })

  test('releases everything that timed out, in one walk, in creation order', () => {
    const { store, create, advance } = storeOn(4)
    const first = create('a')
    advance(1000)
    create('b')
    advance(1000)
    const third = create('c')

    /* Past the timeout of the first two, half a second short of the third's. */
    advance(HARD_TIMEOUT_MS - 500)
    create('d')

    expect(store.size).toBe(2)
    expect(store.get(first.claims, first.token)).toBeUndefined()
    expect(store.get(third.claims, third.token)).toBe(third.session)
  })

  test('never fails, and the ceiling costs the oldest session rather than the newest', () => {
    const { store, create } = storeOn(3)
    const handles = ['a', 'b', 'c', 'd', 'e'].map(create)

    expect(store.size).toBe(3)
    expect(store.slotCount).toBe(3)
    for (const handle of handles.slice(0, 2)) {
      expect(store.get(handle.claims, handle.token)).toBeUndefined()
    }
    for (const handle of handles.slice(2)) {
      expect(store.get(handle.claims, handle.token)).toBe(handle.session)
    }
  })

  // Nobody knows how many sessions a window holds, so the store is not told: it appends a
  // slot when it needs one and stops when the load does.
  test('grows to the load rather than being sized for it', () => {
    const { store, create } = storeOn(1000)
    const handles = Array.from({ length: 50 }, (_, index) => create(String(index)))

    expect(store.size).toBe(50)
    expect(store.slotCount).toBe(50)
    expect(handles.map((handle) => handle.claims.slot)).toEqual(
      Array.from({ length: 50 }, (_, index) => index),
    )
    for (const handle of handles) {
      expect(store.get(handle.claims, handle.token)).toBe(handle.session)
    }
  })

  test('reuses the slots of sessions that have ended instead of growing further', () => {
    const { store, create, advance } = storeOn(1000)
    for (let index = 0; index < 5; index++) {
      create(String(index))
    }
    advance(HARD_TIMEOUT_MS)
    for (let index = 5; index < 10; index++) {
      create(String(index))
    }

    expect(store.size).toBe(5)
    expect(store.slotCount).toBe(5)
  })

  test('a load that keeps arriving does not make the store keep growing', () => {
    const { store, create, advance } = storeOn(1000)
    /* One new session per tenth of the timeout: ten or eleven of them are alive at any
       moment, whatever the process has been doing for the last two hundred of them. */
    let last = create('0')
    for (let index = 1; index < 200; index++) {
      advance(HARD_TIMEOUT_MS / 10)
      last = create(String(index))
    }

    expect(store.size).toBeLessThanOrEqual(11)
    expect(store.slotCount).toBeLessThanOrEqual(11)
    expect(store.get(last.claims, last.token)).toBe(last.session)
  })

  test('nothing is retired early below the ceiling', () => {
    const { store, create } = storeOn(20)
    const handles = Array.from({ length: 20 }, (_, index) => create(String(index)))

    expect(store.size).toBe(20)
    for (const handle of handles) {
      expect(store.get(handle.claims, handle.token)).toBe(handle.session)
    }
  })

  // What reference counting buys the C path, the garbage collector gives for nothing: the
  // store lets go, whoever is mid-request does not, and the session stays correct because
  // it is a disposable view either way.
  test('a session someone is still working with survives being retired', () => {
    const { store, create } = storeOn(1)
    const { claims, token, session } = create('a')
    create('b')

    expect(store.get(claims, token)).toBeUndefined()
    expect(session.name).toBe('session-a')
  })

  test('two sessions created in the same millisecond are still two sessions', () => {
    const { store, create } = storeOn(4)
    const first = create('a')
    const second = create('b')

    expect(first.claims.sessionCreatedAt).toBe(second.claims.sessionCreatedAt)
    expect(store.get(first.claims, first.token)).toBe(first.session)
    expect(store.get(second.claims, second.token)).toBe(second.session)
  })

  // The run is in creation order or expiry is wrong, and the wall clock is the one thing
  // that can step backwards under it -- an NTP correction, a virtual machine resuming.
  test('a clock that steps backwards does not put the run out of order', () => {
    const { store, create, setNow } = storeOn(4)
    const first = create('a')
    setNow(first.claims.sessionCreatedAt - 5_000)
    const second = create('b')

    expect(second.claims.sessionCreatedAt).toBe(first.claims.sessionCreatedAt)
    expect(store.get(first.claims, first.token)).toBe(first.session)
    expect(store.get(second.claims, second.token)).toBe(second.session)
  })
})

describe('SessionStore.invalidate', () => {
  test('ends a session and every token that reached it', () => {
    const { store, create, mint, advance } = storeOn(4)
    const { claims, token } = create('a')
    advance(REISSUE_AFTER_MS)
    const fresh = store.refreshToken(claims, token, mint) as string

    expect(store.invalidate(claims, fresh)).toBe(true)
    expect(store.get(claims, fresh)).toBeUndefined()
    expect(store.get(claims, token)).toBeUndefined()
    expect(store.size).toBe(0)
  })

  test('needs the session´s own token, not merely its claims', () => {
    const { store, create } = storeOn(4)
    const { claims, token, session } = create('a')

    expect(store.invalidate(claims, `${token}.forged`)).toBe(false)
    expect(store.get(claims, token)).toBe(session)
    expect(store.size).toBe(1)
  })

  test('says so when there is nothing to end', () => {
    const { store, create } = storeOn(4)
    const { claims, token } = create('a')
    store.invalidate(claims, token)

    expect(store.invalidate(claims, token)).toBe(false)
  })

  test('holds its slot until expiry reaches it', () => {
    const { store, create } = storeOn(3)
    const first = create('a')
    const second = create('b')
    const third = create('c')
    store.invalidate(second.claims, second.token)

    /* The emptied slot is not free yet, so the store is at its ceiling with nothing free
       and the oldest live session pays for the next one. */
    const fourth = create('d')

    expect(fourth.claims.slot).toBe(first.claims.slot)
    expect(store.size).toBe(2)
    expect(store.get(first.claims, first.token)).toBeUndefined()
    expect(store.get(third.claims, third.token)).toBe(third.session)
  })

  test('the emptied slot comes back when expiry reaches it', () => {
    const { store, create, advance } = storeOn(3)
    const first = create('a')
    const second = create('b')
    store.invalidate(second.claims, second.token)
    create('c')

    advance(HARD_TIMEOUT_MS)
    expect(store.size).toBe(2)
    create('d')

    expect(store.size).toBe(1)
    expect(store.get(first.claims, first.token)).toBeUndefined()
  })
})

describe('SessionStore configuration', () => {
  const base = { hardTimeoutMs: HARD_TIMEOUT_MS, tokenReissueAfterMs: REISSUE_AFTER_MS }

  test('refuses a ceiling that leaves no room for a session', () => {
    for (const maxSessions of [0, -1, 2.5]) {
      expect(() => new SessionStore({ ...base, maxSessions, logger: quietLogger })).toThrow(
        v.ValiError,
      )
    }
  })

  test('starts empty and allocates nothing in advance', () => {
    const store = new SessionStore({ ...base, maxSessions: 100_000, logger: quietLogger })
    expect(store.size).toBe(0)
    expect(store.slotCount).toBe(0)
  })

  test('refuses a timeout that is not one', () => {
    expect(
      () => new SessionStore({ ...base, maxSessions: 4, hardTimeoutMs: 0, logger: quietLogger }),
    ).toThrow(v.ValiError)
  })

  test('refuses a re-issue interval that would not bound the token set', () => {
    expect(
      () =>
        new SessionStore({ ...base, maxSessions: 4, tokenReissueAfterMs: 0, logger: quietLogger }),
    ).toThrow(v.ValiError)
  })
})
