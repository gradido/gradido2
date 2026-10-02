import { t } from '@gradido/frontend-core'
import m from 'mithril'
import { ROUTES } from '../../routes'
import { asset } from '../../utils/asset'
import { RouterLink } from '../RouterLink'

const LOGO = 'img/brand/gradido-logo.png'

export const AuthNavbar: m.Component = {
  view: () =>
    m('.auth-header.position-sticky', [
      m('nav.navbar.navbar-expand.d-flex', [
        // The logo sits on a white blob that overlaps the photo behind it. Both are
        // hidden below lg, where no photo stands beside the form.
        m('.navbar-brand.auth-header-brand.d-none.d-lg-block', [
          m('img.auth-header-logo', {
            src: asset(LOGO),
            width: 200,
            alt: 'Gradido',
          }),
          m('img', {
            src: asset('img/template/gradido_background_header.png'),
            width: 230,
            alt: '',
            loading: 'lazy',
            decoding: 'async',
          }),
        ]),
        // Below lg the logo stands on the page itself, top left.
        m('img.auth-logo-small.d-lg-none', { src: asset(LOGO), alt: 'Gradido' }),
        m(
          '.navbar-collapse.auth-header-collapse',
          m('.navbar-nav.auth-links.auth-navbar.ms-auto.me-lg-4', [
            m(
              '.nav-item',
              m(RouterLink, { href: ROUTES.register, class: 'nav-link' }, t.__('Sign up')),
            ),
            m(
              '.nav-item.separator-start',
              m(RouterLink, { href: ROUTES.login, class: 'nav-link' }, t.__('Sign in')),
            ),
          ]),
        ),
      ]),
    ]),
}
