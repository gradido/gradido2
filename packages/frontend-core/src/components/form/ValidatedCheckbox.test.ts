import { describe, expect, test } from 'bun:test'
import m from 'mithril'
import * as v from 'valibot'
import { FormField } from '../../form/FormField'
import { attrsOf, byTag, classesOf, find, render, textOf } from '../../testing/vnode'
import { ValidatedCheckbox } from './ValidatedCheckbox'

const schema = v.pipe(
  v.boolean(),
  v.check((agreed: boolean) => agreed, 'Please agree to the privacy policy'),
)
const draw = (field: FormField<boolean>) =>
  render(ValidatedCheckbox, { field, name: 'privacy', label: 'I agree' })

const box = (tree: unknown) => find(tree, byTag('input'))

describe('ValidatedCheckbox', () => {
  test('is a checkbox bound to the field', () => {
    const field = new FormField<boolean>(schema, false)
    expect(attrsOf(box(draw(field))).type).toBe('checkbox')
    expect(attrsOf(box(draw(field))).checked).toBe(false)
    field.set(true)
    expect(attrsOf(box(draw(field))).checked).toBe(true)
  })

  test('stays neutral until it is left or revealed', () => {
    const field = new FormField<boolean>(schema, false)
    expect(classesOf(box(draw(field)))).not.toContain('is-invalid')
    field.reveal()
    expect(classesOf(box(draw(field)))).toContain('is-invalid')
    expect(textOf(draw(field))).toContain('Please agree to the privacy policy')
  })

  test('ticking it records the decision', () => {
    const field = new FormField<boolean>(schema, false)
    const onchange = attrsOf(box(draw(field))).onchange as (e: unknown) => void
    onchange({ target: { checked: true } })
    expect(field.value).toBe(true)
    expect(field.valid).toBe(true)
  })

  // The label carries a link to the privacy policy, so it cannot be a plain string.
  test('renders a composed label', () => {
    const field = new FormField<boolean>(schema, false)
    const tree = render(ValidatedCheckbox, {
      field,
      name: 'privacy',
      label: ['I agree to the ', m('a', { href: '/p' }, 'privacy policy')],
    })
    expect(find(tree, byTag('a'))).toBeDefined()
  })
})
