import m from 'mithril'
import eyeIcon from '~icons/bi/eye'
import eyeSlashIcon from '~icons/bi/eye-slash'
import type { FormField } from '../..'
import { t } from '../../i18n'
import { ValidatedInput } from './ValidatedInput'

export interface InputPasswordAttrs {
  field: FormField
  name?: string
  label?: string
  disabled?: boolean
  oninput?: (value: string) => void
}

/** Password input with a show/hide toggle. The toggle is not focusable — it is a
 * convenience, and tabbing from the password field should reach the submit button. */
export class InputPassword implements m.ClassComponent<InputPasswordAttrs> {
  private visible = false

  view({ attrs }: m.Vnode<InputPasswordAttrs>) {
    const toggle = m(
      'button.btn.btn-outline-light.border-start-0.rounded-end.password-toggle',
      {
        type: 'button',
        tabindex: -1,
        'aria-label': this.visible ? t.__('Hide password') : t.__('Show password'),
        onclick: () => {
          this.visible = !this.visible
        },
      },
      m.trust(this.visible ? eyeIcon : eyeSlashIcon),
    )

    return m(ValidatedInput, {
      field: attrs.field,
      name: attrs.name ?? 'password',
      label: attrs.label ?? t.__('Password'),
      type: this.visible ? 'text' : 'password',
      autocomplete: 'current-password',
      disabled: attrs.disabled,
      oninput: attrs.oninput,
      append: toggle,
    })
  }
}
