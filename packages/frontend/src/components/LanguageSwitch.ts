import m, { ClassComponent, Vnode } from 'mithril'

interface LocalePair {
  code: string
  name: string
}

interface LanguageSwitchAttrs {
  locales: LocalePair[]
}

export class LanguageSwitch implements ClassComponent<LanguageSwitchAttrs> {
  private showDropdown = false
  private currentLang: LocalePair | null = null

  private get indexOfSelectedLocale(): number {
    return this.attrs.locales.findIndex(
      (l) => l.code === this.attrs.store.state.language
    )
  }

  private get indexOfLastLocale(): number {
    return this.attrs.locales.length - 1
  }

  private get indexOfSecondLastLocale(): number {
    return this.attrs.locales.length - 2
  }

  oninit(vnode: Vnode<LanguageSwitchAttrs, this>) {
    this.setCurrentLanguage()
  }

  private toggleDropdown() {
    this.showDropdown = !this.showDropdown
  }

  private setCurrentLanguage() {
    const current = this.attrs.locales.find(
      (l) => l.code === this.attrs.store.state.language
    )
    this.currentLang = current || this.attrs.locales[0] || null
  }

  private setLocale(newLocale: string) {
    this.attrs.store.commit('language', newLocale)
    this.currentLang = this.attrs.locales.find((l) => l.code === newLocale) || null
  }

  private async saveLocale(newLocale: string) {
    if (this.currentLang && this.currentLang.code === newLocale) return
    this.setLocale(newLocale)
    if (this.attrs.store.state.gradidoID) {
      try {
        await this.attrs.mutate({ locale: newLocale })
        this.attrs.toastSuccess(this.attrs.t('settings.language.success'))
      } catch {
        this.attrs.toastError(this.attrs.t('settings.language.error'))
      }
    }
  }

  view(vnode: Vnode<LanguageSwitchAttrs, this>) {
    const { locales, store, t } = this.attrs
    return m('div.language-switch', [
      m(
        'div',
        {
          onclick: () => this.toggleDropdown(),
          class: 'pointer d-inline-flex align-items-center'
        },
        [
          locales.map((lang) =>
            m(
              'span',
              {
                key: lang.code,
                class: store.state.language === lang.code ? 'c-grey me-1' : 'c-blau me-1'
              },
              store.state.language === lang.code ? lang.name : ''
            )
          ),
          m('i.bi.bi-caret-down-fill')
        ]
      ),
      this.showDropdown &&
        m('div.mt-4', [
          locales.map((lang, index) =>
            m(
              'span',
              {
                key: lang.code,
                onclick: (e: Event) => {
                  e.preventDefault()
                  this.saveLocale(lang.code)
                  this.toggleDropdown()
                },
                class: store.state.language === lang.code ? 'c-grey' : 'c-blau pointer'
              },
              [
                store.state.language !== lang.code ? lang.name : null,
                store.state.language !== lang.code &&
                (this.indexOfSelectedLocale !== this.indexOfLastLocale ||
                  (this.indexOfSelectedLocale === this.indexOfLastLocale &&
                    index !== this.indexOfSecondLastLocale))
                  ? m('span.ms-3.me-3', locales.length - 1 > index ? t('|') : '')
                  : null
              ]
            )
          )
        ])
    ])
  }
}
