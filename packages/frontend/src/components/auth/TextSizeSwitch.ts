import { t } from 'frontend-core'
import m from 'mithril'
import { asset } from '../../utils/asset'

const SIZES = [0.85, 1, 1.25]

export interface TextSizeSwitchAttrs {
  onselect: (size: number) => void
}

/** Scales the text of the form card, for members who need it larger. */
export class TextSizeSwitch implements m.ClassComponent<TextSizeSwitchAttrs> {
  private open = false

  view({ attrs }: m.Vnode<TextSizeSwitchAttrs>) {
    // A block box holding an inline image, as in legacy: the image sits on the text
    // baseline, so the line box is taller than the image and sets the header row's height.
    return m('.position-relative', [
      m('img.svg-type.pointer', {
        src: asset('img/svg/type.svg'),
        alt: t.__('Text size'),
        role: 'button',
        'aria-expanded': this.open,
        onclick: () => {
          this.open = !this.open
        },
      }),
      this.open
        ? m(
            '.text-size-popover',
            SIZES.map((size, index) =>
              m(
                'span.pointer',
                {
                  key: size,
                  class: index > 0 ? 'separator-start ms-2 ps-2' : undefined,
                  onclick: () => {
                    attrs.onselect(size)
                    this.open = false
                  },
                },
                `${Math.round(size * 100)}%`,
              ),
            ),
          )
        : null,
    ])
  }
}
