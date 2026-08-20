const base = import.meta.env.BASE_URL

/** Resolve a file in `public/` against the configured base path. */
export const asset = (path: string): string => `${base}${path.replace(/^\//, '')}`
