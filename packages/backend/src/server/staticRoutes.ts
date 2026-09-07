import { Elysia, NotFoundError } from 'elysia'

/**
 * One file of a site, with the headers it goes out under.
 *
 * `file` is anything `Bun.file()` can open — in the bundled binary a `/$bunfs/…` path from
 * `import … with { type: 'file' }`. `type` and `etag` are read from `publish/sites.json`
 * rather than derived from the bytes, so the C server answers with the same values.
 */
export type StaticFile = {
  readonly file: string
  /** The `Content-Type` header, charset included. */
  readonly type: string
  /** Unquoted; `send` puts the quotes on. */
  readonly etag: string
  /** The name carries a content hash, so the file may be cached for a year. */
  readonly immutable: boolean
}

/**
 * A built frontend, ready to hand out.
 *
 * `files` is keyed by the path within the site — `assets/index-Ca4jjj45.js`,
 * `locales/de/messages.json`. A map rather than a directory walk: an embedded file has no
 * directory, and `..` is never a key.
 */
export type StaticSite = {
  /** For the startup log, so a binary's sites are visible. */
  readonly name: string
  /** `''` for the domain root, `/admin` for a sub-path. */
  readonly basePath: string
  /** Served for every path the site has no file for. */
  readonly index: StaticFile
  readonly files: ReadonlyMap<string, StaticFile>
}

const IMMUTABLE = 'public, max-age=31536000, immutable'

/** Keep the copy, but revalidate every time — the ETag makes that cheap. */
const REVALIDATE = 'no-cache'

/**
 * The frontends, served from the process that serves the routes they call — same origin, so
 * production needs no CORS.
 *
 * An unmatched path is answered with `index.html` only when the caller sent
 * `Accept: text/html`. A bookmarked `/login` therefore works, while an API client reaches
 * the error handler and the contracted ROUTE_NOT_IMPLEMENTED — see `app.ts`. With no sites
 * nothing is registered at all.
 *
 * `fast-servers/backend/src/static_sites.c` is the same rules in C, on the same manifest.
 */
export function staticRoutes(frontend?: StaticSite, admin?: StaticSite) {
  return new Elysia().use(siteRoutes(frontend)).use(siteRoutes(admin))
}

/**
 * The two routes one site is: its `basePath`, and everything under it.
 *
 * The router ranks `/admin/*` above `/*` by itself, in any registration order, so no site is
 * picked here and `params['*']` is already the path within the site.
 *
 * An absent site registers nothing — a wildcard answering 404 would match every path in the
 * process and take ROUTE_NOT_IMPLEMENTED away from the routes that are not written yet.
 */
function siteRoutes(site?: StaticSite) {
  const app = new Elysia()
  if (!site) {
    return app
  }
  return app
    .get(`${site.basePath}/*`, ({ request, params }) => respond(site, request, params['*']))
    .get(site.basePath === '' ? '/' : site.basePath, ({ request }) => send(site.index, request))
}

/** The file at `relative` — the manifest's own key — or the app, for a caller that takes HTML. */
function respond(site: StaticSite, request: Request, relative: string): Response {
  const file = site.files.get(relative)
  if (file !== undefined) {
    return send(file, request)
  }

  /* `/login` is a route of the mithril app rather than a file, and a bookmark to it has to
     work — but only for a browser. */
  if (!(request.headers.get('accept') ?? '').includes('text/html')) {
    throw new NotFoundError()
  }
  return send(site.index, request)
}

/** The file, or a 304 saying the copy the browser already has is still the file. */
function send(file: StaticFile, request: Request): Response {
  const etag = `"${file.etag}"`
  const headers = {
    'content-type': file.type,
    'cache-control': file.immutable ? IMMUTABLE : REVALIDATE,
    etag,
  }

  if (request.headers.get('if-none-match') === etag) {
    return new Response(null, { status: 304, headers })
  }
  return new Response(Bun.file(file.file), { headers })
}
