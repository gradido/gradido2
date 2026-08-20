import { currentLocale, t } from 'frontend-core'
import m from 'mithril'
import facebookIcon from '~icons/bi/facebook'
import telegramIcon from '~icons/bi/telegram'
import twitterIcon from '~icons/bi/twitter-x'
import youtubeIcon from '~icons/bi/youtube'
import { CONFIG } from '../../config'

const SOCIAL = [
  { href: 'https://www.facebook.com/groups/Gradido/', icon: facebookIcon, name: 'Facebook' },
  { href: 'https://twitter.com/gradido', icon: twitterIcon, name: 'X' },
  { href: 'https://www.youtube.com/c/GradidoNet', icon: youtubeIcon, name: 'YouTube' },
  { href: 'https://t.me/Gradido', icon: telegramIcon, name: 'Telegram' },
]

const external = (href: string, children: m.Children, attrs: Record<string, unknown> = {}) =>
  m('a', { href, target: '_blank', rel: 'noopener noreferrer', ...attrs }, children)

export const AuthFooter: m.Component = {
  view: () => {
    const site = `${CONFIG.WEBSITE_URL}/${currentLocale()}`
    // `mb-5` is clearance, not decoration: the gold bar is fixed to the bottom of the
    // viewport, so without it the last 13px of this footer sit underneath the bar once
    // the page is scrolled to the end.
    return m('footer.auth-footer.pe-5.mb-5', [
      m('.row.mt-4.mt-md-6.mt-lg-7', [
        m(
          '.col-12.col-lg-6',
          m(
            '.d-flex.justify-content-center.justify-content-lg-start.ms-3',
            m('nav.nav.nav-footer', [
              external(`${site}/impressum/`, t.__('Legal notice'), { class: 'nav-link' }),
              external(`${site}/datenschutz/`, t.__('Privacy policy'), { class: 'nav-link' }),
            ]),
          ),
        ),
        m(
          '.col-12.col-lg-6.mt-4.mb-4.mt-lg-0.mb-lg-0',
          m('.d-flex.align-items-center.ms-3.ms-lg-0.text-lg-end.pt-1', [
            t.__('follow us:'),
            // No keys: this array also holds the label text node, and mithril refuses a
            // fragment where only some children are keyed.
            ...SOCIAL.map(({ href, icon, name }) =>
              external(href, m.trust(icon), { class: 'ms-3 c-grey', 'aria-label': name }),
            ),
          ]),
        ),
      ]),
    ])
  },
}
