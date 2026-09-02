// What the extractor cannot read off the pug sources by itself: which variables
// drive a branch, and how C decides that branch at runtime.
//
// Everything else -- the variable list, its order, the struct fields -- is
// derived from the templates themselves, so the pug files stay the single
// source of truth.

import path from 'path'
import { fileURLToPath } from 'url'

// The package root, so the defaults hold wherever node is invoked from.
const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')

// Paths are arguments so that build.zig can collect the output in the build
// cache instead of in the source tree. The defaults are the templates this
// package carries -- imported from gradido's core/src/emails/templates, and the
// single source of truth for both implementations from here on.
const arg = (name, fallback) => {
  const i = process.argv.indexOf('--' + name)
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback
}

export const TEMPLATE_ROOT = path.resolve(arg('templates', path.join(ROOT, 'templates')))
export const LOCALE_DIR = path.resolve(arg('locales', path.join(ROOT, 'locales')))
export const OUT_DIR = path.resolve(arg('out', path.join(ROOT, 'gen')))
// The pug output, checked in. tools/snapshots.mjs writes it, the tests and
// tools/verify.mjs compare against it.
export const SNAPSHOT_DIR = path.resolve(
  arg('snapshots', path.join(ROOT, 'tests', '__snapshots__')),
)
export const IR_PATH = path.resolve(arg('ir', path.join(OUT_DIR, 'ir.json')))

export const LOCALES = ['en', 'de', 'es', 'fr', 'nl', 'it', 'tr', 'ru', 'pt', 'el']

// Marker for a local: replaced by a sentinel when extracting, by a test value
// when verifying.
export const S = (name) => ({ __slot: name })

// GE_HAS(x) == (x != NULL && *x != 0). The argument is the pug variable name;
// the C struct field is its snake_case spelling.
const snake = (s) => s.replace(/([a-z0-9])([A-Z])/g, '$1_$2').toLowerCase()
const has = (f) => `GE_HAS(v->${snake(f)})`

// The `timeDurationObject.minutes == 0` branch from includes/requestNewLink.pug
const durationCond = {
  id: 'duration',
  cases: [
    { c: has('minutes'), locals: { timeDurationObject: { hours: S('hours'), minutes: S('minutes') } } },
    { c: null, locals: { timeDurationObject: { hours: S('hours'), minutes: 0 } } },
  ],
}

export const TEMPLATES = {
  accountActivation: {
    conditions: [
      { id: 'logo', cases: [{ c: has('logoUrl') }, { c: null, locals: { logoUrl: null } }] },
      durationCond,
    ],
  },
  accountMultiRegistration: {
    conditions: [
      { id: 'helper', cases: [{ c: has('helperLink') }, { c: null, locals: { helperLink: null } }] },
    ],
  },
  addedContributionMessage: {},
  assistedRegistrationConfirm: { conditions: [durationCond] },
  contributionChangedByModerator: {},
  contributionConfirmed: {},
  contributionDeleted: {},
  contributionDenied: {},
  customEmail: {
    conditions: [
      {
        id: 'reply',
        cases: [
          { c: `${has('senderCommunityUuid')} && ${has('senderUuid')}` },
          { c: null, locals: { senderCommunityUuid: null, senderUuid: null } },
        ],
      },
    ],
  },
  emailChangeConfirm: {},
  emailChangeDone: {},
  emailChangeNotice: {},
  emailChangeSupport: {
    // Real switches, not strings -> bool fields in the struct
    flags: ['typoCorrection', 'takeBack'],
    conditions: [
      {
        id: 'todo',
        cases: [
          { c: `v->${snake('typoCorrection')}`, locals: { typoCorrection: true, takeBack: false } },
          { c: `v->${snake('takeBack')}`, locals: { typoCorrection: false, takeBack: true } },
          { c: null, locals: { typoCorrection: false, takeBack: false } },
        ],
      },
    ],
  },
  resetPassword: { conditions: [durationCond] },
  thankYouCardPaid: {},
  transactionLinkRedeemed: {},
  transactionReceived: {
    conditions: [
      {
        id: 'reply',
        cases: [
          { c: `${has('senderCommunityUuid')} && ${has('senderUuid')}` },
          { c: null, locals: { senderCommunityUuid: null, senderUuid: null } },
        ],
      },
    ],
  },
}

/*
 * How the HTML becomes the plain text alternative -- html-to-text, at build time, over the
 * *sentinel* HTML, so the slots survive into the text program and C fills them at runtime.
 *
 * Four decisions, and three of them are not cosmetic:
 *
 *   wordwrap off       Reflow would depend on the value in a slot, and then the text a caller
 *                      gets would not be the text this pipeline produced. Every line stays as
 *                      long as its content -- quoted-printable wraps it on the wire anyway.
 *   headings not
 *   uppercased         html-to-text uppercases h1-h6 by default, which would uppercase the
 *                      sentinel with them: \u0001FIRSTNAME\u0002 names no slot this renderer has.
 *   images skipped     Their formatter prints "alt src", and src is a cid: reference that means
 *                      nothing without the HTML part.
 *   .socialmedia
 *   skipped            Four icon links with no text: as plain text they are four bare URLs
 *                      glued together, which is worse than leaving them out.
 */
export const TEXT_OPTIONS = {
  wordwrap: false,
  selectors: [
    { selector: 'img', format: 'skip' },
    { selector: '.socialmedia', format: 'skip' },
    { selector: 'a', options: { hideLinkHrefIfSameAsText: true } },
    ...['h1', 'h2', 'h3', 'h4', 'h5', 'h6'].map((selector) => ({
      selector,
      options: { uppercase: false },
    })),
  ],
}

// Inline attachments (cid:) referenced from layout/header/footer -- these go
// into the binary as bytes.
export const ASSETS = [
  ['gradidoheader', 'gradido-header.png', 'image/png'],
  ['facebookicon', 'facebook-icon.png', 'image/png'],
  ['telegramicon', 'telegram-icon.png', 'image/png'],
  ['twittericon', 'twitter-icon.png', 'image/png'],
  ['youtubeicon', 'youtube-icon.png', 'image/png'],
  ['chatboxicon', 'chatbox-icon.png', 'image/png'],
]

/*
 * The same, for the MJML path. `div` is the difference and it is not a taste:
 * pug marked a paragraph up as <p>, MJML marks it as <div>, and html-to-text gives
 * a <p> a blank line on each side and a <div> only a newline. Without this rule an
 * MJML mail arrives as one dense block.
 *
 * It lives HERE and not in TEXT_OPTIONS because that one is pug's, and pug's text
 * output is the reference tests/__snapshots__ is held to -- changing it there would
 * move 270 snapshots for a reason that has nothing to do with pug.
 */
export const TEXT_OPTIONS_MJML = {
  ...TEXT_OPTIONS,
  selectors: [
    ...TEXT_OPTIONS.selectors,
    { selector: 'div', options: { leadingLineBreaks: 2, trailingLineBreaks: 2 } },
  ],
}
