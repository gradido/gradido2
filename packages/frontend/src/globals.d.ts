// src/types/globals.d.ts
import { i18n } from 'gettext.js'

declare global {
  // global instances
  var t: i18n.Gettext
  var n: (text: string, amount: number) => string
  var toaster: Toaster
  var client: any
}

export {}
