import i18n from 'gettext.js'

globalThis.t = i18n()

function i18nInit() {
  const translationPaths = [
    '../locales/de/messages.json',
    '../locales/en/messages.json'
  ]
  translationPaths.map((path) => fetch(path).then(async (res) => console.log(await res.text())))
  translationPaths.map((path) => fetch(path).then(res => res.json().then((json) => t.loadJSON(json, 'messages'))))
}

export default i18nInit