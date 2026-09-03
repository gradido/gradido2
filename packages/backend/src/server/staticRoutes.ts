import { Elysia, NotFoundError } from 'elysia'

/**
 * A built frontend, ready to be handed out: `frontend` at the domain root, `admin` under
 * `/admin`, and whatever else is compiled into the binary next to them.
 *
 * `files` maps a path *relative to the site's own root* — `assets/index-Ca4jjj45.js`,
 * `locales/de/messages.json` — onto something `Bun.file()` can open. In the bundled binary
 * those are the `/$bunfs/…` paths that `import … with { type: 'file' }` yields, so the whole
 * site travels inside the executable; nothing here knows or cares, which is the point of
 * naming a file rather than a directory.
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
  readonly index: string
  readonly files: ReadonlyMap<string, string>
}

/**
 * Vite writes content-hashed names into this directory and nowhere else, which is what makes
 * a year-long cache safe: the name changes when the bytes do. Everything outside it —
 * `index.html`, `img/`, `locales/` — keeps its name across builds and is revalidated instead.
 */
const HASHED_DIRECTORY = 'assets/'

const IMMUTABLE = 'public, max-age=31536000, immutable'

/** Cache it, but ask every time whether it is still current — the ETag below answers cheaply. */
const REVALIDATE = 'no-cache'

/**
 * The static web server: the frontend and the admin app, out of the same process that serves
 * the routes they call.
 *
 * That is not a convenience, it is the deployment — `Architecture.md` has a gradido2 server as
 * one binary next to a database, so the pages have to come out of it too. It also means the
 * frontend is same-origin with the backend, which is why the session cookie needs no CORS in
 * production and why `API_BASE_URL` is empty in a bundled build.
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

async function respond(
  sites: readonly StaticSite[],
  request: Request,
  path: string,
): Promise<Response> {
  const site = siteFor(sites, path)
  if (site === undefined) {
    throw new NotFoundError()
  }

  const relative = path.slice(site.basePath.length).replace(/^\/+/u, '')

  /* The site's own root is the app, whoever is asking. Not the `Accept` rule below: a bare
     `/` is a person opening the server, not a client that took a wrong turn. */
  if (relative === '') {
    return await send(site.index, REVALIDATE, request)
  }

  const file = site.files.get(relative)
  if (file !== undefined) {
    return await send(file, relative.startsWith(HASHED_DIRECTORY) ? IMMUTABLE : REVALIDATE, request)
  }

  /* No file of that name. `/login` and `/register` are routes of the mithril app rather than
     files, and a bookmark to one has to work — but only for a browser; see the plugin's
     comment for why an API client is handed on instead. */
  if (!(request.headers.get('accept') ?? '').includes('text/html')) {
    throw new NotFoundError()
  }
  return await send(site.index, REVALIDATE, request)
}

/**
 * The file, or a 304 saying the copy the browser already has is still the file.
 *
 * `Bun.file()` fills in the content type from the name and the length from the bytes, for an
 * embedded file exactly as for one on disk.
 */
async function send(file: string, cacheControl: string, request: Request): Promise<Response> {
  const etag = await etagOf(file)
  const headers = { 'cache-control': cacheControl, etag }

  if (request.headers.get('if-none-match') === etag) {
    return new Response(null, { status: 304, headers })
  }
  return new Response(Bun.file(file), { headers })
}

/**
 * Every file's ETag, computed once when it is first asked for.
 *
 * Once and not per request, because the bytes cannot change: they are inside the executable,
 * and a new build is a new process. Lazily and not at startup, because hashing the whole site
 * to serve one page would put the cost on the start rather than on the first visitor — a
 * server with a 130 kB font and four photographs in it should not read them to begin
 * listening.
 */
const etags = new Map<string, string>()

async function etagOf(file: string): Promise<string> {
  const known = etags.get(file)
  if (known !== undefined) {
    return known
  }
  /* Not a cryptographic hash and not meant to be: an ETag says "same bytes as last time" to a
     cache, and nothing downstream trusts it with anything. */
  const etag = `"${Bun.hash(await Bun.file(file).bytes()).toString(16)}"`
  etags.set(file, etag)
  return etag
}
