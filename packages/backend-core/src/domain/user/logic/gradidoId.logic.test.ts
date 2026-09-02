import { describe, expect, test } from 'bun:test'
import { newGradidoId } from './gradidoId.logic'

const never = async () => false

describe('newGradidoId', () => {
  test('draws a v4 uuid', async () => {
    expect(await newGradidoId(never)).toMatch(
      /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/,
    )
  })

  test('asks whether the value is free before handing it over', async () => {
    const asked: string[] = []
    const gradidoId = await newGradidoId(async (candidate) => {
      asked.push(candidate)
      return false
    })
    expect(asked).toEqual([gradidoId])
  })

  // The reason this function exists rather than a bare crypto.randomUUID(): a generated
  // value that collides is drawn again and nobody is told no. An alias cannot do that,
  // which is why its uniqueness is a constraint and this one is a loop.
  test('draws again when the community already holds that id', async () => {
    const asked: string[] = []
    const gradidoId = await newGradidoId(async (candidate) => {
      asked.push(candidate)
      return asked.length === 1
    })

    expect(asked).toHaveLength(2)
    expect(gradidoId).toBe(asked[1])
    expect(gradidoId).not.toBe(asked[0])
  })

  test('gives up rather than spinning when nothing is ever free', async () => {
    expect(newGradidoId(async () => true)).rejects.toThrow(/no free gradido_id/u)
  })
})
