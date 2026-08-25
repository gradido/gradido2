import { t } from '@gradido/frontend-core'
import m from 'mithril'
import { ROUTES } from '../../routes'
import { asset } from '../../utils/asset'
import { RouterLink } from '../RouterLink'

/** Sign-up / sign-in links. Shared with the small navbar shown on narrow screens. */
export const authLinks = (): m.Children => [
  m(RouterLink, { href: ROUTES.register, class: 'nav-link' }, t.__('Sign up')),
  m(
    RouterLink,
    { href: ROUTES.login, class: 'nav-link separator-start ps-3 ms-3' },
    t.__('Sign in'),
  ),
]

export const AuthNavbar: m.Component = {
  view: () =>
    m('.auth-header.position-sticky', [
      m('nav.navbar.d-flex', [
        // The logo sits on a white blob that overlaps the photo behind it. Both are
        // hidden below lg, where the leaves take over, so each breakpoint fetches only
        // its own header art.
        m('.navbar-brand.auth-header-brand.d-none.d-lg-block', [
          m('img.auth-header-logo', {
            src: asset('img/brand/gradido-logo_200x59.png'),
            width: 200,
            alt: 'Gradido',
            loading: 'lazy',
            decoding: 'async',
          }),
          m('img', {
            src: asset('img/template/gradido_background_header.png'),
            width: 230,
            alt: '',
            loading: 'lazy',
            decoding: 'async',
          }),
        ]),
        // Hidden from lg up, but still in the markup: `lazy` is what keeps the browser
        // from fetching it on a desktop that will never show it.
        m('img.auth-header-leaves.position-absolute.d-block.d-lg-none', {
          src: asset('img/template/Blaetter.png'),
          alt: '',
          loading: 'lazy',
          decoding: 'async',
        }),
        m('.navbar-nav.auth-navbar.ms-auto.me-4.d-none.d-lg-flex.flex-row', authLinks()),
      ]),
    ]),
}

/** The same links, for the breakpoints where the header blob is hidden. */
export const AuthNavbarSmall: m.Component = {
  view: () => m('nav.navbar.navi.p-0', m('.navbar-nav.auth-navbar.flex-row', authLinks())),
}
