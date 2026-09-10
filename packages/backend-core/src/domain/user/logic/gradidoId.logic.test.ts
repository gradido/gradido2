import { describe, expect, test } from 'bun:test'
import { newGradidoId } from './gradidoId.logic'

describe('newGradidoId', () => {
  test('draws a v4 uuid', () => {
    expect(newGradidoId()).toMatch(
      /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/u,
    )
  })

  test('draws a different one every time', () => {
    /* The whole guarantee this function makes now that it asks nobody whether the value is
       free: the draws are independent. Whether one is already in the table is answered by
       `users_uuid_key` at the write, and `registerAccount` draws again when it says so. */
    const drawn = new Set(Array.from({ length: 1000 }, () => newGradidoId()))
    expect(drawn.size).toBe(1000)
  })
})
