import {
  currentLocale,
  icon,
  LOCALE_NAMES,
  type Locale,
  SUPPORTED_LOCALES,
  setLocale,
} from '@gradido/frontend-core'
import m from 'mithril'
import caretIcon from '~icons/bi/caret-down-fill'
import checkIcon from '~icons/bi/check'
import globeIcon from '~icons/bi/globe2'

/**
 * Language picker for the pages that are reachable without an account.
 *
 * The choice is only stored locally here. Once a login exists it also has to travel to
 * the account, so that the language a member picked before signing in is the one they
 * keep — that is a backend call and belongs with the login mutation, not here.
 */
export class LanguageSwitch implements m.ClassComponent {
  private open = false
  private readonly onDocumentClick = (event: MouseEvent) => {
    if (!this.open) {
      return
    }
    if (this.root?.contains(event.target as Node)) {
      return
    }
    this.open = false
    m.redraw()
  }
  private root: HTMLElement | undefined

  oncreate({ dom }: m.VnodeDOM) {
    this.root = dom as HTMLElement
    document.addEventListener('click', this.onDocumentClick)
  }

  onremove() {
    document.removeEventListener('click', this.onDocumentClick)
  }

  // Current language first, the rest alphabetically by autonym.
  private ordered(): Locale[] {
    const active = currentLocale()
    const rest = SUPPORTED_LOCALES.filter((locale) => locale !== active).sort((a, b) =>
      LOCALE_NAMES[a].localeCompare(LOCALE_NAMES[b]),
    )
    return [active, ...rest]
  }

  private async select(locale: Locale) {
    this.open = false
    if (locale === currentLocale()) {
      return
    }
    await setLocale(locale, import.meta.env.BASE_URL)
    m.redraw()
  }

  view() {
    const active = currentLocale()
    return m('.language-switch', [
      m(
        'button.ls-trigger',
        {
          type: 'button',
          'aria-expanded': this.open,
          'aria-haspopup': 'listbox',
          onclick: () => {
            this.open = !this.open
          },
        },
        [
          m.trust(icon(globeIcon, 'ls-globe')),
          m('span.ls-current', LOCALE_NAMES[active]),
          m.trust(icon(caretIcon, 'ls-caret')),
        ],
      ),
      this.open
        ? m(
            'ul.ls-menu',
            { role: 'listbox' },
            this.ordered().map((locale) =>
              m(
                `li.ls-item${locale === active ? '.is-active' : ''}`,
                {
                  key: locale,
                  role: 'option',
                  'aria-selected': locale === active,
                  onclick: () => this.select(locale),
                },
                [
                  m('span.ls-item-name', LOCALE_NAMES[locale]),
                  locale === active ? m.trust(icon(checkIcon, 'ls-check')) : null,
                ],
              ),
            ),
          )
        : null,
    ])
  }
}
