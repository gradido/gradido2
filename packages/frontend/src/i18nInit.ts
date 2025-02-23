import i18n from 'gettext.js'
import de from './locales/de/messages.json'
import en from './locales/de/messages.json'

globalThis.t = i18n()
globalThis.n = (text: string, amount: number) => globalThis.t.ngettext(text, text, text, amount)

function i18nInit() {
  const translationPaths = { de, en }
  Object.entries(translationPaths).forEach(([lang, messages]) => {
    t.setMessages('messages', lang, messages)
  })
}

export default i18nInit