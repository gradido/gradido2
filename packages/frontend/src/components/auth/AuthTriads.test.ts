import { describe, expect, test } from 'bun:test'
import { nextTriad } from './AuthTriads'

describe('nextTriad', () => {
  test('moves on to the next triad and wraps round to the first', () => {
    expect(nextTriad(0, 0, 3).index).toBe(1)
    expect(nextTriad(2, 2, 3).index).toBe(0)
  })

  // The stage has to come to rest on the slogan, not on whichever triad was showing
  // when the rounds ran out.
  test('is done after two full rounds, back on the first triad', () => {
    let state = { index: 0, steps: 0, done: false }
    const seen: number[] = []
    while (!state.done) {
      state = nextTriad(state.index, state.steps, 3)
      seen.push(state.index)
    }
    expect(seen).toEqual([1, 2, 0, 1, 2, 0])
  })
})
