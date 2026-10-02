import { t } from '@gradido/frontend-core'
import m from 'mithril'

/** How long one triad stays before the next one slides in. */
const TRIAD_INTERVAL = 7000

/** Two full rounds, then the slogan stays where it is. */
const ROUNDS = 2

/** Length of the slide, the same as `.triad` animates for in `_auth.scss`. */
const SLIDE_DURATION = 600

const triads = () => [
  { name: 'slogan', parts: [t.__('Help.'), t.__('Give.'), t.__('Thank.')] },
  {
    name: 'design',
    parts: [t.__('Community-based.'), t.__('Decentralized.'), t.__('Open Source.')],
  },
  {
    name: 'purpose',
    parts: [t.__('For you and me.'), t.__('For the community.'), t.__('For nature.')],
  },
]

/**
 * The index after `current`, and whether the rounds are used up with it.
 *
 * The last step lands on the first triad again, which is where the stage stays.
 */
export const nextTriad = (
  current: number,
  steps: number,
  count: number,
): { index: number; steps: number; done: boolean } => ({
  index: (current + 1) % count,
  steps: steps + 1,
  done: steps + 1 === ROUNDS * count,
})

const parts = (triad: { parts: string[] }) =>
  triad.parts.map((part) => m('span.triad-part', { key: part }, part))

/** Three short lines of three words each, taking turns above the form. */
export class AuthTriads implements m.ClassComponent {
  private index = 0
  private steps = 0
  private timer: ReturnType<typeof setInterval> | undefined
  /** False until the first triad is on screen: that one is there, it does not slide in. */
  private moving = false

  // Once somebody is in a form field, nothing moves next to what they are typing.
  private readonly stopWhenTyping = (event: FocusEvent) => {
    if (event.target instanceof Element && event.target.closest('input, textarea, select')) {
      this.stop()
    }
  }

  private stop() {
    clearInterval(this.timer)
    this.timer = undefined
    document.removeEventListener('focusin', this.stopWhenTyping)
  }

  private advance(count: number) {
    // A tab in the background does not use up the rounds: they are there to be seen.
    if (document.hidden) {
      return
    }
    const next = nextTriad(this.index, this.steps, count)
    this.index = next.index
    this.steps = next.steps
    this.moving = true
    if (next.done) {
      this.stop()
    }
    m.redraw()
  }

  oncreate() {
    this.timer = setInterval(() => this.advance(triads().length), TRIAD_INTERVAL)
    document.addEventListener('focusin', this.stopWhenTyping)
  }

  onremove() {
    this.stop()
  }

  view() {
    const all = triads()
    const shown = all[this.index]
    return m('.auth-triads.text-center', [
      // Screen readers get the three triads once, not again with every change.
      m('span.visually-hidden', all.flatMap((triad) => triad.parts).join(' ')),
      m('.triad-stage', { 'aria-hidden': true }, [
        // Every triad is laid out once more, invisibly, in the same grid cell as the
        // visible one. The cell takes the height of the tallest, so the form below does
        // not move when one triad replaces another.
        ...all.map((triad) =>
          m('.triad.triad-sizer', { key: `sizer-${triad.name}` }, parts(triad)),
        ),
        m(
          `.triad${this.moving ? '.is-entering' : ''}`,
          {
            key: shown.name,
            // Mithril keeps the element until the promise settles, which is what lets
            // the outgoing triad slide out while the next one slides in.
            onbeforeremove: ({ dom }: m.VnodeDOM) => {
              dom.classList.remove('is-entering')
              dom.classList.add('is-leaving')
              return new Promise((resolve) => setTimeout(resolve, SLIDE_DURATION))
            },
          },
          parts(shown),
        ),
      ]),
    ])
  }
}
