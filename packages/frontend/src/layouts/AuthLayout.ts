import { currentLocale, t, toaster } from '@gradido/frontend-core'
import m from 'mithril'
import { AuthCarousel } from '../components/auth/AuthCarousel'
import { AuthFooter } from '../components/auth/AuthFooter'
import { AuthNavbar, AuthNavbarSmall } from '../components/auth/AuthNavbar'
import { LanguageSwitch } from '../components/auth/LanguageSwitch'
import { TextSizeSwitch } from '../components/auth/TextSizeSwitch'
import { CONFIG } from '../config'
import { asset } from '../utils/asset'

/**
 * Frame around every page that is reachable without an account.
 *
 * The photo column is `position: fixed` on purpose: the form column is the only thing
 * that scrolls, so a long form does not drag the photo out of the viewport.
 */
export class AuthLayout implements m.ClassComponent {
  private fontSize = 1

  view({ children }: m.Vnode) {
    return m('.auth-template', [
      m(AuthNavbar),

      m('.auth-aside.position-fixed.d-none.d-lg-block', [
        m('.auth-aside-image.position-absolute.w-100', m(AuthCarousel)),
        m(
          '.auth-aside-cta.position-relative.text-center',
          m(
            'a.btn.btn-gradido',
            { href: `${CONFIG.WEBSITE_URL}/${currentLocale()}`, target: '_blank', rel: 'noopener' },
            t.__('Learn more …'),
          ),
        ),
      ]),

      m(
        '.row.justify-content-md-center.justify-content-lg-end',
        m('.col-12.col-md-8.col-lg-6.auth-column', [
          m('.ms-3.ms-sm-4.me-3.me-sm-4', [
            // Between md and lg the header blob is hidden, so the links move here.
            m('.row.d-none.d-md-block.d-lg-none', m('.col.auth-navbar', m(AuthNavbarSmall))),

            m('.row.mt-0.mt-md-5.ps-2.ps-md-0', [
              m('.col-12.col-md-9', [
                m('.mb-n2', t.__('Welcome to the community')),
                m('.h1.mb-0', CONFIG.COMMUNITY_NAME),
                m('div', t.__('1000 thanks for being with us!')),
              ]),
              m(
                '.col-3.text-end.d-none.d-md-block',
                m('img.auth-coin.rounded-circle', {
                  src: asset('img/brand/gradido_coin_128x128.png'),
                  alt: '',
                  loading: 'lazy',
                  decoding: 'async',
                }),
              ),
            ]),

            m(
              '.card.border-0.mt-4.gradido-custom-background.page-font-size',
              { style: { fontSize: `${this.fontSize}rem` } },
              [
                m('.row.p-4', [
                  m('.col-10', m('.ms-3', m(LanguageSwitch))),
                  m(
                    '.col-2.text-end',
                    m(TextSizeSwitch, {
                      onselect: (size: number) => {
                        this.fontSize = size
                      },
                    }),
                  ),
                ]),

                // Below md the coin and the links sit inside the card instead.
                m(
                  '.row.d-md-none.mb-3',
                  m('.col.text-center', [
                    m('img.auth-coin.rounded-circle', {
                      src: asset('img/brand/gradido_coin_128x128.png'),
                      alt: '',
                      loading: 'lazy',
                      decoding: 'async',
                    }),
                    m('.d-flex.justify-content-center.auth-navbar.mt-2', m(AuthNavbarSmall)),
                  ]),
                ),

                m('.card-body', children),
              ],
            ),
          ]),
          m(AuthFooter),
        ]),
      ),

      // App chrome rather than layout: it closes off every page, and moves to the app
      // shell once there is a second layout.
      m('.goldrand.position-fixed.fixed-bottom'),

      m('.toaster-wrapper', m(toaster)),
    ])
  }
}
