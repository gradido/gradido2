import { describe, expect, test } from 'bun:test'
import { stubIcons } from '@gradido/frontend-core/testing/icons'

stubIcons()
const { nextSlide } = await import('./AuthCarousel')

describe('nextSlide', () => {
  test('advances when the next photo has arrived', () => {
    expect(nextSlide(0, 3, new Set([0, 1]))).toBe(1)
    expect(nextSlide(1, 3, new Set([0, 1, 2]))).toBe(2)
  })

  test('wraps round to the first photo', () => {
    expect(nextSlide(2, 3, new Set([0, 1, 2]))).toBe(0)
  })

  // Without this the carousel would cross-fade to a panel with no photo behind it.
  test('holds the current photo while the next one is still loading', () => {
    expect(nextSlide(0, 3, new Set([0]))).toBeUndefined()
    expect(nextSlide(1, 3, new Set([0, 1]))).toBeUndefined()
  })

  test('holds rather than repeat itself when only the current photo exists', () => {
    expect(nextSlide(0, 1, new Set([0]))).toBe(0)
  })
})
