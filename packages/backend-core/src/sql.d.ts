/**
 * `.sql` files are imported as text — see `database/migrate/migrations.ts` for why the
 * migrations are imported rather than read from disk.
 */
declare module '*.sql' {
  const content: string
  // biome-ignore lint/style/noDefaultExport: bun's shape for a text import, not a choice of ours
  export default content
}
