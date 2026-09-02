import {
  currentLocale,
  Form,
  FormField,
  InputEmail,
  t,
  toaster,
  ValidatedCheckbox,
  ValidatedInput,
} from '@gradido/frontend-core'
import { ErrorCode } from '@gradido/shared/errors'
import {
  emailPrevalidateSchema,
  emailSchema,
  firstNamePrevalidateSchema,
  firstNameSchema,
  lastNamePrevalidateSchema,
  lastNameSchema,
  privacyConsentSchema,
} from '@gradido/shared/schemas'
import m from 'mithril'
import { RegisterError, register } from '../client'
import { Message, RouterLink } from '../components'
import { CONFIG } from '../config'
import { ROUTES } from '../routes'

/**
 * The consent sentence, with its link.
 *
 * Translators get one whole sentence with the linked part marked, rather than three
 * fragments they cannot reorder — and the markup stays out of the message catalog, where
 * it would be an injection surface rather than a translation.
 */
const consentLabel = (): m.Children => {
  const [before, linkText, after] = t.__('I agree to the [privacy policy].').split(/[[\]]/)
  return [
    before,
    m(
      'a',
      {
        href: `${CONFIG.WEBSITE_URL}/${currentLocale()}/datenschutz/`,
        target: '_blank',
        rel: 'noopener noreferrer',
      },
      linkText,
    ),
    after,
  ]
}

export class Register implements m.ClassComponent {
  private readonly form = new Form({
    firstName: new FormField({
      prevalidate: firstNamePrevalidateSchema,
      validate: firstNameSchema,
    }),
    lastName: new FormField({ prevalidate: lastNamePrevalidateSchema, validate: lastNameSchema }),
    email: new FormField({ prevalidate: emailPrevalidateSchema, validate: emailSchema }),
    privacy: new FormField<boolean>(privacyConsentSchema, false),
  })
  private submitting = false
  private registered = false

  private async submit(event: Event) {
    event.preventDefault()
    this.form.reveal()
    if (!this.form.valid || this.submitting) {
      return
    }

    this.submitting = true
    try {
      const { firstName, lastName, email } = this.form.values()
      await register({ firstName, lastName, email, language: currentLocale() })
      this.registered = true
    } catch (error) {
      this.handleError(error)
    } finally {
      this.submitting = false
      m.redraw()
    }
  }

  /**
   * A registration that came back with an address already in use looks exactly like one
   * that did not — the server does not say, on purpose — so there is no such case here.
   * What is left is a server that cannot answer at all.
   */
  private handleError(error: unknown) {
    const code = error instanceof RegisterError ? error.code : ErrorCode.Unknown
    switch (code) {
      case ErrorCode.RouteNotImplemented:
        // The deployment runs an implementation that does not serve this route, and
        // nothing forwards it elsewhere — see Architecture.md, One implementation per
        // deployment. Retrying is pointless, so say so rather than offering hope.
        toaster.error(t.__('This server does not offer registration yet.'))
        break
      case ErrorCode.ValidationFailed:
        // The form validated first, so reaching this means the two rule sets disagree.
        toaster.error(t.__('Please check your entries.'))
        break
      default:
        toaster.error(`${t.__('Unknown error: ')}${(error as Error).message}`)
    }
  }

  view() {
    // Nothing to go back to: the account exists but cannot be used until the address is
    // confirmed, so the form is replaced rather than left standing.
    if (this.registered) {
      return m(Message, {
        headline: t.__('Thank you!'),
        subtitle: t.__(
          'You are registered now, please check your emails and click the activation link.',
        ),
      })
    }

    const valid = this.form.valid
    return m('.register-form.container', [
      m('.pb-5.text-center', t.__('Community-based – Decentralized – Open Source')),

      m('form', { onsubmit: (event: Event) => this.submit(event) }, [
        m('.row', [
          m(
            '.col-12.col-md-6',
            m(ValidatedInput, {
              field: this.form.fields.firstName,
              name: 'firstname',
              label: t.__('First name'),
              autocomplete: 'given-name',
              class: 'mb-3',
              disabled: this.submitting,
            }),
          ),
          m(
            '.col-12.col-md-6',
            m(ValidatedInput, {
              field: this.form.fields.lastName,
              name: 'lastname',
              label: t.__('Last name'),
              autocomplete: 'family-name',
              class: 'mb-3',
              disabled: this.submitting,
            }),
          ),
        ]),

        m(
          '.row',
          m('.col', m(InputEmail, { field: this.form.fields.email, disabled: this.submitting })),
        ),

        m(
          '.row',
          m(
            '.col-12.my-4',
            m(ValidatedCheckbox, {
              field: this.form.fields.privacy,
              name: 'privacy',
              label: consentLabel(),
              disabled: this.submitting,
            }),
          ),
        ),

        m(
          '.row',
          m(
            // The handler sits on the wrapper, not the button: a disabled button fires no
            // mouse events, and reaching for it is exactly when the reason it is disabled
            // needs to be on screen.
            '.col-12.col-lg-6',
            { onmouseenter: () => this.form.reveal() },
            m(
              `button.btn.${valid ? 'btn-gradido' : 'btn-gradido-disable'}`,
              {
                type: 'submit',
                disabled: !valid || this.submitting,
                'data-test': 'submit-register',
              },
              this.submitting ? t.__('Registering …') : t.__('Sign up'),
            ),
          ),
        ),

        m('.row', m('.col.mt-3', t.__('Already have a %1 account?', CONFIG.COMMUNITY_NAME))),
        m(
          '.row',
          m('.col.mt-1.auth-navbar', m(RouterLink, { href: ROUTES.login }, t.__('Sign in'))),
        ),
      ]),
    ])
  }
}
