import { describe, expect, test } from 'bun:test'
import m from 'mithril'
import * as v from 'valibot'
import { FormField } from '../../form/FormField'
import { attrsOf, byTag, classesOf, find, render, textOf } from '../../testing/vnode'
import { ValidatedInput } from './ValidatedInput'

const schema = v.pipe(v.string(), v.nonEmpty('This field is required'))
const draw = (field: FormField, extra: Record<string, unknown> = {}) =>
  render(ValidatedInput, { field, name: 'email', label: 'Email', ...extra })

const input = (tree: unknown) => find(tree, byTag('input'))
const feedback = (tree: unknown) =>
  find(tree, (vnode) => classesOf(vnode).includes('invalid-feedback'))

describe('validation state reaches the markup', () => {
  test('a neutral field carries neither validity class', () => {
    const classes = classesOf(input(draw(new FormField(schema))))
    expect(classes).toContain('form-control')
    expect(classes).not.toContain('is-valid')
    expect(classes).not.toContain('is-invalid')
  })

  test('a valid field is marked valid', () => {
    expect(classesOf(input(draw(new FormField(schema, 'x'))))).toContain('is-valid')
  })

  test('a field shown as invalid carries the class and the message', () => {
    const field = new FormField(schema)
    field.touch()
    expect(classesOf(input(draw(field)))).toContain('is-invalid')
    expect(textOf(feedback(draw(field)))).toBe('This field is required')
  })

  test('a neutral field keeps its message box empty', () => {
    expect(textOf(feedback(draw(new FormField(schema))))).toBe('')
  })
})

describe('wiring', () => {
  test('typing records the value and reports it onward', () => {
    const field = new FormField(schema)
    const seen: string[] = []
    const attrs = attrsOf(input(draw(field, { oninput: (value: string) => seen.push(value) })))
    ;(attrs.oninput as (e: unknown) => void)({ target: { value: 'ein ' } })
    expect(field.value).toBe('ein ')
    expect(seen).toEqual(['ein '])
  })

  test('leaving the field marks it touched', () => {
    const field = new FormField(schema)
    ;(attrsOf(input(draw(field))).onblur as () => void)()
    expect(field.state).toBe(false)
  })

  test('label and message are tied to the input for screen readers', () => {
    const tree = draw(new FormField(schema))
    const attrs = attrsOf(input(tree))
    expect(attrs.id).toBe('email-input-field')
    expect(attrs['aria-describedby']).toBe('email-feedback')
    expect(attrsOf(find(tree, byTag('label'))).for).toBe('email-input-field')
  })

  test('an appended control moves the field into an input group', () => {
    const tree = draw(new FormField(schema), { append: m('button', { type: 'button' }) })
    expect(find(tree, (vnode) => classesOf(vnode).includes('input-group'))).toBeDefined()
  })
})
