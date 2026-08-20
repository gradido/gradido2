import { describe, expect, test } from 'bun:test'
import * as v from 'valibot'
import { FormField } from '../../form/FormField'
import { attrsOf, byTag, deepRender, find, render } from '../../testing/vnode'
import { InputEmail } from './InputEmail'

const draw = () =>
  render(InputEmail, { field: new FormField(v.pipe(v.string(), v.nonEmpty('required'))) })

const input = () => attrsOf(find(deepRender(draw()), byTag('input')))

describe('InputEmail', () => {
  // `type="email"` runs a value sanitization algorithm that strips leading and trailing
  // whitespace before any handler sees the value. A typed space therefore never reaches
  // validation, and could only be reported one keystroke later, once the next character
  // had turned it into an interior one. `inputmode` keeps the address keyboard on touch
  // devices without that behavior.
  test('is a text input with an email inputmode, never type="email"', () => {
    expect(input().type).toBe('text')
    expect(input().inputmode).toBe('email')
  })

  test('does not offer the previous member’s address on a shared device', () => {
    expect(input().autocomplete).toBe('off')
  })

  test('is addressable by the name the login page and its tests use', () => {
    expect(input().id).toBe('email-input-field')
    expect(input()['data-test']).toBe('input-email')
  })
})
