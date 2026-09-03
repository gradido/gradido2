import { Elysia, NotFoundError } from 'elysia'

/**
 * One file of a site, and everything needed to hand it out.
 *
 * `file` is something `Bun.file()` can open. In the bundled binary those are the `/$bunfs/…`
 * paths that `import … with { type: 'file' }` yields, so the whole site travels inside the
 * executable; nothing here knows or cares, which is the point of naming a file rather than a
 * directory.
 *
 * The type and the ETag are **not** worked out here. They come from `publish/sites.json`,
 * where `scripts/publish.ts` wrote them, and the C server reads the same values out of the
 * same file — so what a client gets does not depend on which implementation is deployed. It
 * also means no request ever hashes anything: the bytes were fixed when the build ran.
 */
export type StaticFile = {
  readonly file: string
  /** The `Content-Type` header, complete with charset where one belongs. */
  readonly type: string
  /** The ETag, without the quotes this server puts around it on the wire. */
  readonly etag: string
  /** Whether the name carries a content hash, and the file may be cached for a year. */
  readonly immutable: boolean
}

/**
 * A built frontend, ready to be handed out: `frontend` at the domain root, `admin` under
 * `/admin`, and whatever else is compiled into the binary next to them.
 *
 * `files` is keyed by the path *relative to the site's own root* — `assets/index-Ca4jjj45.js`,
 * `locales/de/messages.json`.
 *
 * A map and not a directory walk, for two reasons that are the same reason: an embedded file
 * has no directory to walk, and a lookup that can only answer with a key somebody put in the
 * map cannot be talked into `../../etc/passwd`. Path traversal is not defended against below
 * because there is nothing to defend — `..` is simply not a key.
 */
export type StaticSite = {
  /** For the startup log, so it is visible which sites a binary carries. */
  readonly name: string
  /** Where the site is mounted: `''` for the domain root, `/admin` for a sub-path. */
  readonly basePath: string
  /** The app's entry document, served for every path the site does not have a file for. */
  readonly index: StaticFile
  readonly files: ReadonlyMap<string, StaticFile>
}

const IMMUTABLE = 'public, max-age=31536000, immutable'

/** Cache it, but ask every time whether it is still current — the ETag answers cheaply. */
const REVALIDATE = 'no-cache'

/**
 * The static web server: the frontend and the admin app, out of the same process that serves
 * the routes they call.
 *
 * That is not a convenience, it is the deployment — `Architecture.md` has a gradido2 server as
 * one binary next to a database, so the pages have to come out of it too. It also means the
 * frontend is same-origin with the backend, which is why the session cookie needs no CORS in
 * production and why `API_BASE_URL` is empty in a published build.
 *
 * **What it must not do is answer for the routes.** Nearly every path of `contracts/server`
 * is still unwritten, and the contract requires those to say `ROUTE_NOT_IMPLEMENTED` rather
 * than 404 — see `app.ts`. A wildcard that returned `index.html` for everything would turn
 * every one of them into an HTML page, silently, and a client would parse the app as its
 * answer. So an unmatched path is only answered with the app when whoever asked wants HTML:
 * a browser navigating to `/login` sends `Accept: text/html`, an API client does not, and no
 * cleverness is needed to tell them apart. Everything else is handed on as `NotFoundError`
 * and comes back out of the app's error handler as the contracted answer.
 *
 * Mounted with no sites — `bun src/index.ts`, where the frontend is served by vite on its own
 * port — every path takes that same road, so the plugin changes nothing about a server that
 * carries no pages.
 *
 * `fast-servers/backend/src/static_sites.c` is the same rules in C, against the same manifest.
 */
export const staticRoutes = (sites: readonly StaticSite[]) =>
  new Elysia({
    name: 'gradido.static',
    /* Elysia deduplicates plugins by name, so the seed says which sites this one carries:
       two static plugins with different sites are two plugins, not one. */
    seed: sites.map((site) => site.basePath).join('|'),
  })
    /* `/*` does not match the bare root in Elysia's router, so the site root is its own
       route. Both go to the same handler, which is what makes them the same rule. */
    .get('/', ({ request, path }) => respond(sites, request, path))
    .get('/*', ({ request, path }) => respond(sites, request, path))

/** The site a path belongs to: the longest `basePath` it starts with, so `/admin` wins over `''`. */
function siteFor(sites: readonly StaticSite[], path: string): StaticSite | undefined {
  let match: StaticSite | undefined
  for (const site of sites) {
    if (site.basePath !== '' && path !== site.basePath && !path.startsWith(`${site.basePath}/`)) {
      continue
    }
    if (match === undefined || site.basePath.length > match.basePath.length) {
      match = site
    }
  }
  return match
}

function respond(sites: readonly StaticSite[], request: Request, path: string): Response {
  const site = siteFor(sites, path)
  if (site === undefined) {
    throw new NotFoundError()
  }

  const relative = path.slice(site.basePath.length).replace(/^\/+/u, '')

  /* The site's own root is the app, whoever is asking. Not the `Accept` rule below: a bare
     `/` is a person opening the server, not a client that took a wrong turn. */
  if (relative === '') {
    return send(site.index, request)
  }

  const file = site.files.get(relative)
  if (file !== undefined) {
    return send(file, request)
  }

  /* No file of that name. `/login` and `/register` are routes of the mithril app rather than
     files, and a bookmark to one has to work — but only for a browser; see the plugin's
     comment for why an API client is handed on instead. */
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
