import m from 'mithril'
import type { FormField } from '../..'
import { t } from '../../i18n'
import { ValidatedInput } from './ValidatedInput'

export interface InputEmailAttrs {
  field: FormField
  name?: string
  disabled?: boolean
  oninput?: (value: string) => void
}

export const InputEmail: m.Component<InputEmailAttrs> = {
  view: ({ attrs }) =>
    m(ValidatedInput, {
      field: attrs.field,
      name: attrs.name ?? 'email',
      label: t.__('Email'),
      // Not `type="email"`: that type's value sanitization strips leading and trailing
      // whitespace before the value is ever seen, so a typed space could only be
      // reported one keystroke later, once the next character made it an interior one.
      // `inputmode` still gets the address keyboard on touch devices.
      type: 'text',
      inputmode: 'email',
      // Off on purpose: a shared device should not offer the previous member's address.
      autocomplete: 'off',
      disabled: attrs.disabled,
      oninput: attrs.oninput,
    }),
}
