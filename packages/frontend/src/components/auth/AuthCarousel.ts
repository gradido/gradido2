import { t } from 'frontend-core'
import m from 'mithril'
import { asset } from '../../utils/asset'

const SLIDE_INTERVAL = 13000

/** How long before a photo is due to start fetching it. */
const PRELOAD_LEAD = 3000

const slides = () => [
  {
    image: 'img/template/Foto_01_2400_small.jpg',
    title: t.__('Gratitude'),
    subtitle: t.__('For each other, for all people, for nature.'),
  },
  {
    image: 'img/template/Foto_05_2400_small.jpg',
    title: t.__('Dignity'),
    subtitle: t.__('We gift to each other and give thanks with Gradido.'),
  },
  {
    image: 'img/template/Foto_04_2400_small.jpg',
    title: t.__('Donation'),
    subtitle: t.__('You are a gift for the community. 1000 thanks because you are with us.'),
  },
]

/**
 * The slide to move to, or `undefined` to hold the current one.
 *
 * Holding is the important half: fading to a photo that has not arrived would show an
 * empty panel, which looks broken in a way that a photo lingering a few seconds longer
 * does not.
 */
export const nextSlide = (
  current: number,
  count: number,
  ready: ReadonlySet<number>,
): number | undefined => {
  const next = (current + 1) % count
  return ready.has(next) ? next : undefined
}

/**
 * Cross-fading photo carousel.
 *
 * Written by hand rather than driven by Bootstrap's carousel: that one moves classes
 * around on the slide elements, which is exactly the DOM mithril re-derives on every
 * redraw. Owning the state keeps the two from overwriting each other.
 *
 * The three photos are close to a megabyte together, and most visitors sign in long
 * before the second one is due. So only the visible photo is part of the page load; the
 * next is fetched shortly before its turn, and a photo that has not finished loading is
 * simply not switched to — the carousel waits a beat rather than fading to a blank
 * panel. Someone who signs in within ten seconds never pays for the other two.
 */
export class AuthCarousel implements m.ClassComponent {
  private index = 0
  /** Slides whose photo is decoded and safe to show. The first one loads with the page. */
  private ready = new Set([0])
  private pending = new Set<number>()
  private timers: ReturnType<typeof setTimeout>[] = []

  private preload(index: number) {
    if (this.ready.has(index) || this.pending.has(index)) {
      return
    }
    this.pending.add(index)
    const image = new Image()
    image.onload = () => {
      this.pending.delete(index)
      this.ready.add(index)
      m.redraw()
    }
    image.onerror = () => this.pending.delete(index)
    image.src = asset(slides()[index].image)
  }

  oncreate() {
    const count = slides().length
    this.timers.push(setTimeout(() => this.preload(1), SLIDE_INTERVAL - PRELOAD_LEAD))
    this.timers.push(
      setInterval(() => {
        const next = nextSlide(this.index, count, this.ready)
        if (next === undefined) {
          // Still loading. Hold this photo and try again on the next turn.
          this.preload((this.index + 1) % count)
          return
        }
        this.index = next
        this.preload((next + 1) % count)
        m.redraw()
      }, SLIDE_INTERVAL) as unknown as ReturnType<typeof setTimeout>,
    )
  }

  onremove() {
    for (const timer of this.timers) {
      clearTimeout(timer)
      clearInterval(timer as unknown as ReturnType<typeof setInterval>)
    }
  }

  view() {
    const all = slides()
    const current = all[this.index]
    return m('.auth-carousel', [
      m(
        '.auth-carousel-inner',
        all.map((slide, index) =>
          m(`.auth-carousel-slide${index === this.index ? '.is-active' : ''}`, {
            key: slide.image,
            // An unfetched photo carries no url, so it never reaches the network as part
            // of the page load.
            style: this.ready.has(index)
              ? { backgroundImage: `url(${asset(slide.image)})` }
              : undefined,
            'aria-hidden': index !== this.index,
          }),
        ),
      ),
      m('.auth-carousel-caption', [
        m('.auth-carousel-title', current.title),
        m('.auth-carousel-subtitle', current.subtitle),
      ]),
    ])
  }
}
