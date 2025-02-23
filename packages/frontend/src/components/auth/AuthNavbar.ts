import { BNavItem } from '../bootstrap/BNavItem'
import m, { Component } from 'mithril'

const backgroundHeader = '/img/template/gradido_background_header.png'
const logo = '/img/brand/gradido-logo_200x59.png'
const sheet = '/img/template/Blaetter.png'

export const AuthNavbar: Component = {
  view: () =>
    m('div.auth-header.position-sticky', [
      m('nav.d-flex.navbar.navbar-expand', [
        m('div.navbar-brand.d-none.d-lg-block', [
          m('img.position-absolute.p-2', {
            src: logo, width: '200', alt: 'Logo'
          }),
          m('img', {
            src: backgroundHeader, width: '230', alt: 'Background Image',
          }),
        ]),
        m('img.sheet-img.position-absolute.d-block.d-lg-none.zindex1000', {
          src: sheet,
        }),
        m('div#nav-collapse.collapse.navbar-collapse', { 'is-nav': 'true' }, [
          m('ul.navbar-nav.ms-auto.me-4.d-none.d-lg-flex', [
            m(BNavItem, { classes: ['ms-lg-5'], href: '/register' }, t.__('Sign Up')),
            m('span.d-none.d-lg-block.py-1', '|'),
            m(BNavItem, { href: '/login' }, t.__('Sign In'))
          ]),
        ]),
      ]),
    ]),
}
