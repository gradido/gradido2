import { beforeAll, describe, expect, it } from 'bun:test'
import { mkdtemp } from 'node:fs/promises'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { Elysia } from 'elysia'
import { type StaticFile, type StaticSite, staticRoutes } from './staticRoutes'

/**
 * The static server, against files on disk.
 *
 * The bundled binary hands it embedded paths instead, and that is the same thing as far as
 * this code is concerned: both are opened with `Bun.file`, which is the whole reason the site
 * is a map of paths rather than a directory. What is worth testing is the part that is not
 * file reading — which path gets the app, which one is handed on, and what a second site under
 * a sub-path does to both.
 */

/** The app under test, as the only thing the tests need of it: a request in, a response out. */
let handle: (request: Request) => Promise<Response>

const INDEX = '<!doctype html><title>frontend</title>'
const ADMIN_INDEX = '<!doctype html><title>admin</title>'

beforeAll(async () => {
  const directory = await mkdtemp(join(tmpdir(), 'gradido-static-'))
  /* What `publish/sites.json` carries per file, minus the parts this test does not vary. */
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

  /* A bare Elysia and not `createBackendApp`: what the app makes of a NotFoundError is the
     error contract's business and is tested where that contract is. Here 404 means only
     "the static server did not answer this", which is the decision under test. */
  const app = new Elysia().use(staticRoutes([frontend, admin]))
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
    /* The manifest's value, quoted — not something this server computed. */
    expect(first.headers.get('etag')).toBe('"ccc"')

    const second = await get('/locales/de/messages.json', { 'if-none-match': '"ccc"' })
    expect(second.status).toBe(304)
    expect(await second.text()).toBe('')
  })

  it('serves the type the manifest gave it', async () => {
    /* Not what Bun infers from the extension: the same string the C server reads out of the
       same manifest, so a client cannot tell the two implementations apart. */
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
    /* The contracted ROUTE_NOT_IMPLEMENTED lives at the other end of this: 139 routes are
       unwritten, and answering an API client with the login page would hide every one. */
    expect((await get('/user/login', { accept: 'application/json' })).status).toBe(404)
    expect((await get('/assets/gone.js')).status).toBe(404)
  })

  it('has no file for a path that traverses out of the site', async () => {
    expect((await get('/assets/../../../etc/passwd')).status).toBe(404)
  })

  it('prefers the site with the longer base path', async () => {
    expect(await (await get('/admin')).text()).toBe(ADMIN_INDEX)
    expect(await (await get('/admin/settings', { accept: 'text/html' })).text()).toBe(ADMIN_INDEX)
    expect((await get('/admin/assets/admin-def456.js')).status).toBe(200)
    /* Not the admin's file, even though the name is the admin's: it was asked for at the
       root site, which does not have it. */
    expect((await get('/assets/admin-def456.js')).status).toBe(404)
  })
})
