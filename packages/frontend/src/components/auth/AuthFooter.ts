import m from 'mithril'
import FacebookIcon from '~icons/simple-icons/facebook'
import TwitterXIcon from '~icons/simple-icons/x'
import YouTubeIcon from '~icons/simple-icons/youtube'
import TelegramIcon from '~icons/simple-icons/telegram'

interface AuthFooterAttrs {
  className?: string
}

export const AuthFooter: m.Component<AuthFooterAttrs> = {
  view: ({attrs}) => {
    const locale = localStorage.getItem('language')
    return m('footer.footer', { className: attrs.className }, [
      m('.row.mt-lg-7.mt-md-6.mt-4', [
        m('.col-12.col-md-12.col-lg-6', [
          m(
            '.d-flex.justify-content-center.justify-content-md-center.justify-content-lg-start.ms-3',
            m('ul.nav.nav-footer', [
              m('li.nav-item',
                m('a.nav-link.nav-link-black', {
                  href: `https://gradido.net/${locale}/impressum/`,
                  target: '_blank',
                }, t.__('Legal notice'))
              ),
              m('li.nav-item',
                m('a.nav-link.nav-link-black', {
                  href: `https://gradido.net/${locale}/datenschutz/`,
                  target: '_blank',
                }, t.__('Privacy policy'))
              ),
            ])
          ),
        ]),
        m('.col-12.col-md-12.col-lg-6.mt-4.mb-4.mt-lg-0.mb-lg-0', [
          m('.d-flex.align-items-center.ms-3.ms-lg-0.text-lg-end.pt-1', [
            t.__('follow us:'),
            m('a',
              { href: 'https://www.facebook.com/groups/Gradido/', target: '_blank' },
              m('.ms-3.me-3.c-grey', m.trust(FacebookIcon)),
            ),
            m('a',
              { href: 'https://x.com/gradido', target: '_blank' },
              m('.me-3.c-grey', m.trust(TwitterXIcon)),
            ),
            m('a',
              { href: 'https://www.youtube.com/c/GradidoNet', target: '_blank' },
              m('.me-3.c-grey', m.trust(YouTubeIcon)),
            ),
            m('a', 
              { href: 'https://t.me/Gradido', target: '_blank' }, 
              m('.c-grey', m.trust(TelegramIcon)),
            ),
          ]),
        ]),
      ]),
    ])
  },
}
