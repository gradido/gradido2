import { Form, FormField, InputEmail, InputPassword, t, toaster } from 'frontend-core'
import m from 'mithril'
import { emailPrevalidateSchema, emailSchema, loginPasswordSchema } from 'shared'
import { LoginError, LoginErrorCode, login } from '../client/authClient'
import { Message } from '../components/Message'
import { RouterLink } from '../components/RouterLink'
import { CONFIG } from '../config'
import { ROUTES } from '../routes'

interface PageMessage {
  subtitle: string
  linkTo: string
}

export class Login implements m.ClassComponent {
  private readonly form = new Form({
    email: new FormField({ prevalidate: emailPrevalidateSchema, validate: emailSchema }),
    password: new FormField(loginPasswordSchema),
  })
  private submitting = false
  private pageMessage: PageMessage | undefined

  private async submit(event: Event) {
    event.preventDefault()
    this.form.reveal()
    if (!this.form.valid || this.submitting) {
      return
    }

    this.submitting = true
    try {
      await login(this.form.values())
      // TODO: session handling and the redirect back to where the member came from
      // land with the backend route — see client/authClient.ts.
      toaster.success(t.__('Signed in'))
    } catch (error) {
      this.handleError(error)
    } finally {
      this.submitting = false
      m.redraw()
    }
  }

  /**
   * Two of these failures are not really errors: the account exists but cannot be used
   * yet. Those replace the form, because retrying the same credentials cannot help —
   * the member has to activate the account or set a password first.
   */
  private handleError(error: unknown) {
    const code = error instanceof LoginError ? error.code : LoginErrorCode.Unknown
    switch (code) {
      case LoginErrorCode.EmailNotValidated:
        this.pageMessage = {
          subtitle: t.__(
            'Your account has not been activated yet. Please check your email and click the activation link, or request a new one on the password reset page.',
          ),
          linkTo: ROUTES.forgotPassword,
        }
        toaster.error(t.__('We could not find an activated account with this data.'))
        break
      case LoginErrorCode.NoPasswordSet:
        this.pageMessage = {
          subtitle: t.__('No password has been set for this account yet.'),
          linkTo: ROUTES.forgotPassword,
        }
        toaster.error(t.__('We could not find an activated account with this data.'))
        break
      case LoginErrorCode.InvalidCredentials:
        toaster.error(t.__('No user with these credentials.'))
        break
      default:
        toaster.error(`${t.__('Unknown error: ')}${(error as Error).message}`)
    }
  }

  view() {
    if (this.pageMessage) {
      return m(Message, {
        headline: t.__('Attention!'),
        subtitle: this.pageMessage.subtitle,
        buttonText: t.__('Reset password'),
        linkTo: this.pageMessage.linkTo,
      })
    }

    const valid = this.form.valid
    return m('.login-form.container', [
      m('.pb-5.text-center', t.__('Community-based – Decentralized – Open Source')),

      m('form', { onsubmit: (event: Event) => this.submit(event) }, [
        m('.row', [
          m(
            '.col-12.col-lg-6',
            m(InputEmail, { field: this.form.fields.email, disabled: this.submitting }),
          ),
          m(
            '.col-12.col-lg-6',
            m(InputPassword, { field: this.form.fields.password, disabled: this.submitting }),
          ),
        ]),

        m(
          '.row',
          m(
            '.col.d-flex.justify-content-end.mb-4.mb-lg-0',
            m(
              RouterLink,
              { href: ROUTES.forgotPassword, 'data-test': 'forgot-password-link' },
              t.__('Forgot password?'),
            ),
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
              `button.btn.w-100.fs-7.${valid ? 'btn-gradido' : 'btn-gradido-disable'}`,
              { type: 'submit', disabled: !valid || this.submitting, 'data-test': 'submit-login' },
              this.submitting ? t.__('Signing in …') : t.__('Sign in'),
            ),
          ),
        ),

        m('.row', m('.col.mt-3', t.__('Don’t have a %1 account yet?', CONFIG.COMMUNITY_NAME))),
        m(
          '.row',
          m('.col.mt-1.auth-navbar', m(RouterLink, { href: ROUTES.register }, t.__('Sign up'))),
        ),
      ]),
    ])
  }
}
