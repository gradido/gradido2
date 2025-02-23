import '../assets/scss/components/auth.scss'
import m from 'mithril'
import { CONFIG } from '../config'
import { AuthNavbar } from '../components/auth/AuthNavbar'
import { AuthCarousel } from '../components/auth/AuthCarousel'
import { BPopover } from '../components/bootstrap/BPopover'
import { AuthFooter } from '../components/auth/AuthFooter'
import AaIcon from '~icons/mdi/format-letter-case'

interface AuthLayoutState {
  project: string
  projectBannerResult: { projectBrandingBanner?: string } | null
  projectBannerLoading: boolean
  textSize: number
}

export const AuthLayout: m.Component = {
  oninit: (vnode) => {
    const state = vnode.state as AuthLayoutState
    state.project = ''
    state.projectBannerResult = null
    state.projectBannerLoading = false
    state.textSize = 1

    const urlParams = new URLSearchParams(window.location.search)
    const projectValue = urlParams.get('project') || ''
    state.project = projectValue

    // Example: fetch or refetch GraphQL data here
    // state.projectBannerLoading = true
    /*someAsyncFetchProjectBanner(projectValue).then((result) => {
      state.projectBannerResult = result
      state.projectBannerLoading = false
      m.redraw()
    })*/
  },
  view: (vnode) => {
    const state = vnode.state as AuthLayoutState

    const setTextSize = (size: number) => {
      state.textSize = size
      document.querySelector<HTMLElement>('.page-font-size')!.style.fontSize = size + 'rem'
    }
    const locale = localStorage.getItem('language')

    return m('.auth-template.overflow-x-hidden', [
      m('.h-100.align-middle', [
        m(AuthNavbar),
        m('.left-content-box.position-fixed.d-none.d-lg-block', [
          m('.bg-img-box.position-absolute.w-100', m(AuthCarousel)),
          m('.bg-txt-box.position-relative.d-none.d-lg-block.text-center.align-self-center', [
            m('a', { href: 'https://gradido.net/' + locale, target: '_blank' }, [
              m('button.btn.btn-gradido', t.__('Learn more …'))
            ])
          ])
        ]),
        m('.row.justify-content-md-center.justify-content-lg-end', [
          m('.col-sm-12.col-md-8.col-lg-6.zindex1000', [
            m('.ms-3.ms-sm-4.me-3.me-sm-4', [
              m('.row.d-none.d-md-block.d-lg-none', [
                m('.col.mb--4', [m('auth-navbar-small')])
              ]),
              state.projectBannerLoading || state.projectBannerResult
                ? m('.row.d-none.d-md-block', [
                    m('.col', [
                      state.projectBannerResult &&
                        m('img.img-fluid', {
                          src: state.projectBannerResult.projectBrandingBanner,
                          alt: 'project banner'
                        })
                    ])
                  ])
                : m('.row.mt-0.mt-md-5.ps-2.ps-md-0.ps-lg-0', [
                    m('.col-lg-9.col-md-9.col-sm-12', [
                      m('div.mb--2', t.__('Welcome to the community')),
                      m('div.h1.mb-0', CONFIG.COMMUNITY_NAME),
                      m('div.mb-0', t.__('1000 thanks for being with us!'))
                    ]),
                    m('.col-3.text-end.d-none.d-sm-none.d-md-inline', [
                      m('img', {
                        src: '/img/brand/gradido_coin_128x128.png',
                        style: { width: '6rem', borderRadius: '50%' }
                      })
                    ])
                  ]),
              m('.card.border-0.mt-4.gradido-custom-background.page-font-size', [
                m('.row.p-4', [
                  m('.col-10', [m('language-switch-2.ms-3')]),
                  m('.col-2.text-end',
                    m(BPopover, { id: 'popover-target-1', trigger: 'click', darkMode: true, title: m.trust(AaIcon)}, [
                        m('span.pointer.me-2', { onclick: () => setTextSize(0.85) }, n('%1%%', 85)),
                        m('span.me-2', '|'),
                        m('span.pointer.me-2', { onclick: () => setTextSize(1) }, n('%1%%', 100)),
                        m('span.me-2', '|'),
                        m('span.pointer', { onclick: () => setTextSize(1.25) }, n('%1%%', 125))
                      ]
                    )
                  )
                ]),
                m('.row.d-inline.d-sm-inline.d-md-none.d-lg-none.mb-3', [
                  m('.col.text-center', [
                    m('img', {
                      src: '/img/brand/gradido_coin_128x128.png',
                      style: { width: '6rem', borderRadius: '50%' }
                    }),
                    m('.row', [m('.col.zindex1000.d-flex.justify-content-center', [m('auth-navbar-small')])])
                  ])
                ]),
                m('.card-body', [
                  // router-view placeholder
                  m('router-view')
                ])
              ])
            ]),
            // show footer if route meta doesn't hide it, placeholder logic
            m(AuthFooter, { className: '.pe-5.mb-5' })
          ])
        ])
      ])
    ])
  }
}
