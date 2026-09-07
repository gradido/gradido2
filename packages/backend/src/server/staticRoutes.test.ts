import { beforeAll, describe, expect, it } from 'bun:test'
import { mkdtemp } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { Elysia } from 'elysia'
import { type StaticFile, type StaticSite, staticRoutes } from './staticRoutes'

/**
 * Two sites over files on disk. The bundled binary hands in `/$bunfs/…` paths instead, which
 * `Bun.file` opens the same way, so only the routing is under test here.
 */

/** The app under test: a request in, a response out. */
let handle: (request: Request) => Promise<Response>

const INDEX = '<!doctype html><title>frontend</title>'
const ADMIN_INDEX = '<!doctype html><title>admin</title>'

beforeAll(async () => {
  const directory = await mkdtemp(join(tmpdir(), 'gradido-static-'))
  /* One file's entry in `publish/sites.json`, minus what these tests do not vary. */
  const write = async (
    name: string,
    content: string,
    rest: Omit<StaticFile, 'file'>,
  ): Promise<StaticFile> => {
    const path = join(directory, name)
    await Bun.write(path, content)
    return { file: path, ...rest }
  }
  const html = { type: 'text/html; charset=utf-8', immutable: false }

  const frontend: StaticSite = {
    name: 'frontend',
    basePath: '',
    index: await write('index.html', INDEX, { ...html, etag: 'aaa' }),
    files: new Map([
      ['index.html', await write('index2.html', INDEX, { ...html, etag: 'aaa' })],
      [
        'assets/app-abc123.js',
        await write('app.js', 'console.log(1)', {
          type: 'text/javascript; charset=utf-8',
          etag: 'bbb',
          immutable: true,
        }),
      ],
      [
        'locales/de/messages.json',
        await write('messages.json', '{"hello":"hallo"}', {
          type: 'application/json; charset=utf-8',
          etag: 'ccc',
          immutable: false,
        }),
      ],
    ]),
  }

  const admin: StaticSite = {
    name: 'admin',
    basePath: '/admin',
    index: await write('admin.html', ADMIN_INDEX, { ...html, etag: 'ddd' }),
    files: new Map([
      [
        'assets/admin-def456.js',
        await write('admin.js', 'console.log(2)', {
          type: 'text/javascript; charset=utf-8',
          etag: 'eee',
          immutable: true,
        }),
      ],
    ]),
  }

  /* A bare Elysia, not `createBackendApp`: 404 here means only that the static server did
     not answer. What the app makes of that is tested with the error contract. */
  const app = new Elysia().use(staticRoutes(frontend, admin))
  handle = async (request) => await app.handle(request)
})

const get = (path: string, headers: Record<string, string> = {}): Promise<Response> =>
  handle(new Request(`http://localhost${path}`, { headers }))

describe('staticRoutes', () => {
  it('serves the app at the site root, whoever is asking', async () => {
    const response = await get('/')
    expect(response.status).toBe(200)
    expect(await response.text()).toBe(INDEX)
  })

  it('serves a file by its path', async () => {
    const response = await get('/locales/de/messages.json')
    expect(response.status).toBe(200)
    expect(response.headers.get('content-type')).toContain('application/json')
    expect(await response.json()).toEqual({ hello: 'hallo' })
  })

  it('lets a hashed asset be cached for a year, and nothing else', async () => {
    expect((await get('/assets/app-abc123.js')).headers.get('cache-control')).toBe(
      'public, max-age=31536000, immutable',
    )
    expect((await get('/locales/de/messages.json')).headers.get('cache-control')).toBe('no-cache')
  })

  it('answers a revalidation with 304 and no body', async () => {
    const first = await get('/locales/de/messages.json')
    /* The manifest's value, quoted. */
    expect(first.headers.get('etag')).toBe('"ccc"')

    const second = await get('/locales/de/messages.json', { 'if-none-match': '"ccc"' })
    expect(second.status).toBe(304)
    expect(await second.text()).toBe('')
  })

  it('serves the type the manifest gave it', async () => {
    /* The manifest's string, not what Bun infers from the extension — the C server reads
       the same one. */
    expect((await get('/assets/app-abc123.js')).headers.get('content-type')).toBe(
      'text/javascript; charset=utf-8',
    )
  })

  it('gives a browser the app for a route of the app', async () => {
    const response = await get('/login', { accept: 'text/html' })
    expect(response.status).toBe(200)
    expect(await response.text()).toBe(INDEX)
  })

  it('hands an unmatched path on when the caller does not want HTML', async () => {
    /* Answering an API client with the login page would hide every unwritten route. */
    expect((await get('/user/login', { accept: 'application/json' })).status).toBe(404)
    expect((await get('/assets/gone.js')).status).toBe(404)
  })

  it('has no file for a path that traverses out of the site', async () => {
    expect((await get('/assets/../../../etc/passwd')).status).toBe(404)
  })

  it('lets the router pick the site, by how specific the route is', async () => {
    expect(await (await get('/admin')).text()).toBe(ADMIN_INDEX)
    expect(await (await get('/admin/settings', { accept: 'text/html' })).text()).toBe(ADMIN_INDEX)
    expect((await get('/admin/assets/admin-def456.js')).status).toBe(200)
    /* Asked for at the root site, which does not have it. */
    expect((await get('/assets/admin-def456.js')).status).toBe(404)
    /* The prefix only counts at a segment boundary, so this is the root site's. */
    expect(await (await get('/administration', { accept: 'text/html' })).text()).toBe(INDEX)
  })
})

describe('staticRoutes with no sites', () => {
  /* A wildcard answering 404 here would take ROUTE_NOT_IMPLEMENTED away from every
     unwritten route — see app.ts. */
  it("registers nothing, so every path is still the app's to answer", async () => {
    const app = new Elysia()
      .onError(({ code }) => (code === 'NOT_FOUND' ? 'not implemented' : 'other'))
      .use(staticRoutes())

    for (const path of ['/', '/login', '/admin/settings']) {
      const response = await app.handle(new Request(`http://localhost${path}`))
      expect(await response.text()).toBe('not implemented')
    }
  })
})
