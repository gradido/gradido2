import '../../assets/scss/components/carousel.scss'
import m, { Component } from 'mithril'
import Carousel from 'bootstrap/js/src/carousel'

export const AuthCarousel: Component = {
  oncreate: () => {
    // Initialize the carousel
    const carousel = document.getElementById('welcome-carousel')
    if (carousel) {
      new Carousel(carousel, {
        interval: 13000,
        ride: 'carousel',
        pause: 'hover', // pause on mouse hover
        keyboard: false,
      })
    }
  },
  view: () => {
    return m('#welcome-carousel.carousel.slide', [
      // You could integrate a carousel library or plugin here.
      m('div.carousel-inner', [
        m('div.carousel-item.active', [
          m('img.b-img.d-block.w-100', { src: '/img/template/Foto_01_2400_small.jpg' }),
          m('.carousel-caption', [
            m('.caption-first-text', t.__('Gratitude')),
            m('.caption-second-text', t.__('For each other, for all people, for nature.')),
          ]),
        ]),
        m('div.carousel-item', [
          m('img', { src: '/img/template/Foto_02_2400_small.jpg' }),
          m('.carousel-caption', [
            m('.caption-first-text', t.__('Dignity')),
            m('.caption-second-text', t.__('We gift to each other and give thanks with Gradido.')),
          ]),
        ]),
        m('div.carousel-item', [
          m('img', { src: '/img/template/Foto_03_2400_small.jpg' }),
          m('.carousel-caption', [
            m('.caption-first-text', t.__('Donation')),
            m('.caption-second-text', t.__('You are a gift for the community. 1000 thanks because you are with us.')),
          ]),
        ]),
      ]),
    ])
  },
}
