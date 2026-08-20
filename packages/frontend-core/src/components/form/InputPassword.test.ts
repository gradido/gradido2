import { describe, expect, test } from 'bun:test'
import type m from 'mithril'
import * as v from 'valibot'
import { FormField } from '../../form/FormField'
import { stubIcons } from '../../testing/icons'
import { attrsOf, byTag, deepRender, find } from '../../testing/vnode'

stubIcons()

const { InputPassword } = await import('./InputPassword')

const attrs = { field: new FormField(v.pipe(v.string(), v.nonEmpty('required'))) }
const draw = (component: InstanceType<typeof InputPassword>) =>
  component.view({ attrs } as m.Vnode<typeof attrs>)

const inputType = (tree: unknown) => attrsOf(find(deepRender(tree), byTag('input'))).type
const toggle = (tree: unknown) => attrsOf(find(deepRender(tree), byTag('button')))

describe('InputPassword', () => {
  test('hides the password until the toggle is used', () => {
    const component = new InputPassword()
    expect(inputType(draw(component)))?.toBe('password')
    ;(toggle(draw(component)).onclick as () => void)()
    expect(inputType(draw(component))).toBe('text')
    ;(toggle(draw(component)).onclick as () => void)()
    expect(inputType(draw(component))).toBe('password')
  })

  // The toggle is a convenience: tabbing out of the password field should reach the
  // submit button, not a show/hide control.
  test('keeps the toggle out of the tab order and labels it for screen readers', () => {
    const component = new InputPassword()
    expect(toggle(draw(component)).tabindex).toBe(-1)
    expect(toggle(draw(component))['aria-label']).toBe('Show password')
    ;(toggle(draw(component)).onclick as () => void)()
    expect(toggle(draw(component))['aria-label']).toBe('Hide password')
  })
})
