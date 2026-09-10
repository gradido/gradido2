import { describe, expect, test } from 'bun:test'
import * as v from 'valibot'
import {
  type DatabaseCheckable,
  databaseConfigSchema,
  isDatabasePasswordAcceptable,
  isUnixSocketHost,
} from './schema'

/**
 * The two rules of `contracts/database-config.json`, which are the reason that file exists: they
 * are read from the environment by both implementations and must come out the same. The C path
 * asserts the identical cases in `fast-servers/service-core/tests/test_db.cpp` — when one of
 * these moves, the other has to move with it or an operator learns the rule twice.
 */
describe('isUnixSocketHost', () => {
  test('a leading slash is what selects a socket, and nothing else', () => {
    expect(isUnixSocketHost('/var/run/postgresql')).toBe(true)
    expect(isUnixSocketHost('/tmp')).toBe(true)
    expect(isUnixSocketHost('localhost')).toBe(false)
    expect(isUnixSocketHost('127.0.0.1')).toBe(false)
    expect(isUnixSocketHost('db.example.org')).toBe(false)
    expect(isUnixSocketHost('')).toBe(false)
  })
})

describe('databaseConfigSchema', () => {
  test('defaults to a Unix socket rather than to a host', () => {
    const config = v.parse(databaseConfigSchema, {})
    /* contracts/database-config.json, DB_HOST. The default reaches a database on this machine
       the faster of the two ways; a deployment whose database is elsewhere names it. */
    expect(config.DB_HOST).toBe('/var/run/postgresql')
    expect(isUnixSocketHost(config.DB_HOST)).toBe(true)
  })

  /* contracts/database-config.json, DB_POOL_SIZE -- the same cases as test_db.cpp's
     APoolSizeThatIsNotACountIsRefused, because a value one path starts with and the other
     refuses is a deployment that works until somebody switches implementation. */
  test('holds ten connections unless told otherwise', () => {
    expect(v.parse(databaseConfigSchema, {}).DB_POOL_SIZE).toBe(10)
    expect(v.parse(databaseConfigSchema, { DB_POOL_SIZE: '48' }).DB_POOL_SIZE).toBe(48)
    expect(v.parse(databaseConfigSchema, { DB_POOL_SIZE: '1' }).DB_POOL_SIZE).toBe(1)
    /* Set but empty is not decided, which is what `DB_POOL_SIZE=` in an .env says. */
    expect(v.parse(databaseConfigSchema, { DB_POOL_SIZE: '' }).DB_POOL_SIZE).toBe(10)
  })

  test('refuses a pool size that is not a count of at least one', () => {
    for (const wrong of ['0', '-4', 'ten', '10x', '65536']) {
      expect(v.safeParse(databaseConfigSchema, { DB_POOL_SIZE: wrong }).success).toBe(false)
    }
  })
})

describe('isDatabasePasswordAcceptable', () => {
  /* Everything the rule refuses, so that each case below changes exactly one thing away from it. */
  const refused: DatabaseCheckable = {
    DB_TYPE: 'postgresql',
    DB_HOST: 'db.example.org',
    DB_PASSWORD: '',
    NODE_ENV: 'production',
  }

  test('refuses an empty password over TCP in production', () => {
    expect(isDatabasePasswordAcceptable(refused)).toBe(false)
  })

  test('accepts it over a Unix socket', () => {
    /* No network for the database to answer on: the reach is a filesystem permission and peer
       authentication, where no password is the correct configuration and not an oversight. */
    expect(isDatabasePasswordAcceptable({ ...refused, DB_HOST: '/var/run/postgresql' })).toBe(true)
    expect(isDatabasePasswordAcceptable({ ...refused, DB_HOST: '/tmp' })).toBe(true)
  })

  test('the default host is exempt, because the default is that socket', () => {
    const config = v.parse(databaseConfigSchema, {})
    expect(isDatabasePasswordAcceptable({ ...refused, DB_HOST: config.DB_HOST })).toBe(true)
  })

  test('accepts a password, a non-production environment, and SQLite', () => {
    expect(isDatabasePasswordAcceptable({ ...refused, DB_PASSWORD: 'something' })).toBe(true)
    expect(isDatabasePasswordAcceptable({ ...refused, NODE_ENV: 'development' })).toBe(true)
    /* SQLite is a file and has no password to be empty. */
    expect(isDatabasePasswordAcceptable({ ...refused, DB_TYPE: 'sqlite' })).toBe(true)
  })
})
