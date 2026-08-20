// po2json ships no types; only the one call this repo makes is declared.
declare module 'po2json' {
  export function parseFileSync(
    file: string,
    options?: { format?: 'raw' | 'jed' | 'jed1.x' | 'mf' },
  ): Record<string, unknown>
  const po2json: { parseFileSync: typeof parseFileSync }
  export default po2json
}
