import { afterAll, describe, expect, test } from 'bun:test'
import { existsSync, mkdtempSync, readFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join, resolve } from 'node:path'
import { deriveDhtNodeKeyPair } from '@gradido/shared/crypto'
import { drive } from '../../../scripts/pty'

/**
 * `setup` on both paths, end to end, on a pseudo-terminal: the development proposal taken with
 * Enter, SQLite chosen where the machine also has a PostgreSQL, a few keys typed for the master
 * seed. What it must leave behind is what every later start reads -- the `.env` with the peer
 * network identity, and a database with a community. Run by `bun run test:e2e` at the root.
 */

const ROOT = resolve(import.meta.dir, '../../..')
const FAST = resolve(process.env.GRADIDO2_FAST ?? join(ROOT, 'fast-servers/zig-out/bin/gradido2-fast'))
const workdirs: string[] = []

afterAll(() => {
  for (const dir of workdirs) {
    rmSync(dir, { recursive: true, force: true })
  }
})

/** What somebody at the terminal answers: Enter, but SQLite and a few keys. */
function answer(screen: string): string | undefined {
  if (screen.includes('random keys')) {
    return 'e2e keys\r'
  }
  /* The choice list marks the proposal with ❯; SQLite is the entry above PostgreSQL. */
  if (screen.includes('Which database?') && /❯ \S*postgresql/.test(screen)) {
    return '\x1b[A\r'
  }
  return undefined
}

function readEnv(path: string): Record<string, string> {
  const env: Record<string, string> = {}
  for (const line of readFileSync(path, 'utf8').split('\n')) {
    const match = /^([A-Z_]+)=(.*)$/.exec(line)
    if (match?.[1] !== undefined) {
      env[match[1]] = match[2] ?? ''
    }
  }
  return env
}

async function setUp(command: readonly string[]): Promise<{ dir: string; env: Record<string, string> }> {
  const dir = mkdtempSync(join(tmpdir(), 'gradido2-setup-'))
  workdirs.push(dir)
  const result = await drive(command, {
    cwd: dir,
    /* Nothing of the test's own environment: NODE_ENV=test or a DB_TYPE would change the proposal. */
    env: { PATH: process.env.PATH ?? '', HOME: process.env.HOME ?? '', TERM: 'xterm' },
    answer,
  })
  if (result.exitCode !== 0) {
    throw new Error(`setup exited with ${result.exitCode}:\n${result.output}`)
  }
  return { dir, env: readEnv(join(dir, '.env')) }
}

function expectIdentity(env: Record<string, string>): void {
  expect(env.DB_TYPE).toBe('sqlite')
  expect(env.MASTER_SEED).toMatch(/^[0-9a-f]{64}$/)
  expect(env.DHT_DELEGATION).toMatch(/^[0-9a-f]{272}$/)
  /* http://localhost is no public URL. */
  expect(env.DHT_REACHABILITY).toBe('private')
  /* The delegation names the node the master seed derives, which is what the dht-node checks. */
  const node = deriveDhtNodeKeyPair(new Uint8Array(Buffer.from(env.MASTER_SEED ?? '', 'hex')))
  expect(env.DHT_DELEGATION?.slice(0, 64)).toBe(Buffer.from(node.publicKey).toString('hex'))
}

describe('setup', () => {
  test('on the reference path leaves the peer network identity in .env', async () => {
    const { dir, env } = await setUp(['bun', join(ROOT, 'packages/backend/src/index.ts'), 'setup'])
    expectIdentity(env)
    expect(existsSync(join(dir, env.DB_FILE ?? ''))).toBe(true)
  }, 60_000)

  test('on the fast path does the same', async () => {
    if (!existsSync(FAST)) {
      throw new Error(`${FAST} does not exist: run \`bun run zig build\` first`)
    }
    const { dir, env } = await setUp([FAST, 'setup'])
    expectIdentity(env)
    expect(existsSync(join(dir, env.DB_FILE ?? ''))).toBe(true)
  }, 60_000)
})
