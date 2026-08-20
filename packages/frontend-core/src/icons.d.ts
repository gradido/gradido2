// unplugin-icons with `compiler: 'raw'` — every icon is the SVG markup as a string.
declare module '~icons/*' {
  const markup: string
  export default markup
}
