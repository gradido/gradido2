import { afterEach, beforeEach, describe, expect, test } from 'bun:test'
import { mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { resolveSecrets, SECRET_VARIABLES, secretSource } from './secret'

/**
 * The resolution order of `contracts/secrets.json`, which the C path asserts case for case in
 * `fast-servers/service-core/tests/test_env.cpp`. When one of these moves the other has to move
 * with it: an operator who has learned the mechanism on one binary has learned it on both, or
 * the mechanism is not one.
 */
describe('resolveSecrets', () => {
  let dir = ''

  beforeEach(() => {
    dir = mkdtempSync(join(tmpdir(), 'gradido-secret-'))
  })
  afterEach(() => {
    rmSync(dir, { recursive: true, force: true })
  })

  const write = (name: string, content: string): string => {
    const path = join(dir, name)
    writeFileSync(path, content)
    return path
  }

  test('takes the variable when nothing else names a source', () => {
    expect(resolveSecrets({ DB_PASSWORD: 'from-env' }).DB_PASSWORD).toBe('from-env')
  })

  test('a file the environment names beats the variable', () => {
    const path = write('pw', 'from-file')
    expect(resolveSecrets({ DB_PASSWORD: 'from-env', DB_PASSWORD_FILE: path }).DB_PASSWORD).toBe(
      'from-file',
    )
  })

  test('a systemd credential beats both', () => {
    writeFileSync(join(dir, 'DB_PASSWORD'), 'from-credential')
    const path = write('pw', 'from-file')
    expect(
      resolveSecrets({
        DB_PASSWORD: 'from-env',
        DB_PASSWORD_FILE: path,
        CREDENTIALS_DIRECTORY: dir,
      }).DB_PASSWORD,
    ).toBe('from-credential')
  })

  test('a credentials directory without this credential falls through', () => {
    /* A unit loads the credentials it needs and no others, so an absent file is ordinary. */
    expect(
      resolveSecrets({ DB_PASSWORD: 'from-env', CREDENTIALS_DIRECTORY: dir }).DB_PASSWORD,
    ).toBe('from-env')
  })

  test('one trailing line ending goes and nothing else', () => {
    expect(resolveSecrets({ DB_PASSWORD_FILE: write('a', 'secret\n') }).DB_PASSWORD).toBe('secret')
    expect(resolveSecrets({ DB_PASSWORD_FILE: write('b', 'secret\r\n') }).DB_PASSWORD).toBe(
      'secret',
    )
    /* Only one, and only at the end: a password may legitimately hold or end in whitespace, and
       one nobody can express is worse than one that needs a careful printf. */
    expect(resolveSecrets({ DB_PASSWORD_FILE: write('c', 'secret\n\n') }).DB_PASSWORD).toBe(
      'secret\n',
    )
    expect(resolveSecrets({ DB_PASSWORD_FILE: write('d', ' secret ') }).DB_PASSWORD).toBe(
      ' secret ',
    )
  })

  test('an empty file is an answer, not an absence', () => {
    expect(
      resolveSecrets({ DB_PASSWORD: 'from-env', DB_PASSWORD_FILE: write('e', '') }).DB_PASSWORD,
    ).toBe('')
  })

  test('a named file that cannot be read is fatal rather than a fallback', () => {
    /* The rule the whole order stands on: a silent fallback turns an unreadable secret into an
       empty one, and an empty password is how a process connects as somebody else. */
    expect(() =>
      resolveSecrets({ DB_PASSWORD: 'from-env', DB_PASSWORD_FILE: join(dir, 'nope') }),
    ).toThrow(/refusing to fall back/)
  })

  test('every declared secret resolves the same way', () => {
    /* The mechanism is one mechanism or it is not one: an operator who has learned it on
       DB_PASSWORD has learned it on EMAIL_PASSWORD. This walks SECRET_VARIABLES rather than
       naming them, so a secret added to the list without being wired fails here. */
    for (const name of SECRET_VARIABLES) {
      expect(resolveSecrets({ [name]: 'from-env' })[name]).toBe('from-env')
      expect(
        resolveSecrets({
          [name]: 'from-env',
          [`${name}_FILE`]: write(`${name}.pw`, 'from-file'),
        })[name],
      ).toBe('from-file')
      writeFileSync(join(dir, name), 'from-credential')
      expect(
        resolveSecrets({
          [name]: 'from-env',
          [`${name}_FILE`]: write(`${name}.pw`, 'from-file'),
          CREDENTIALS_DIRECTORY: dir,
        })[name],
      ).toBe('from-credential')
    }
  })

  test('says which source answered, so a caller can decline to write it down', () => {
    /* `setup` offers the current value as a default and writes the answer into .env. For a
       secret systemd keeps on a tmpfs that would be a downgrade, so setup asks where the value
       came from first and leaves the stronger two alone. */
    writeFileSync(join(dir, 'DB_PASSWORD'), 'x')
    expect(secretSource('DB_PASSWORD', { CREDENTIALS_DIRECTORY: dir })).toBe('credential')
    expect(secretSource('DB_PASSWORD', { DB_PASSWORD_FILE: '/anywhere' })).toBe('file')
    expect(secretSource('DB_PASSWORD', { DB_PASSWORD: 'x' })).toBe('environment')
    expect(secretSource('DB_PASSWORD', {})).toBeUndefined()
    /* A credentials directory without this credential is not a credential source. */
    expect(
      secretSource('EMAIL_PASSWORD', { CREDENTIALS_DIRECTORY: dir, EMAIL_PASSWORD: 'x' }),
    ).toBe('environment')
  })

  test('the source it reports is the source it reads from', () => {
    /* The two walks must not drift: whichever one says wins has to be the one that answered. */
    writeFileSync(join(dir, 'DB_PASSWORD'), 'from-credential')
    const env = {
      DB_PASSWORD: 'from-env',
      DB_PASSWORD_FILE: write('pw', 'from-file'),
      CREDENTIALS_DIRECTORY: dir,
    }
    expect(secretSource('DB_PASSWORD', env)).toBe('credential')
    expect(resolveSecrets(env).DB_PASSWORD).toBe('from-credential')
  })

  test('leaves everything that is not a secret alone', () => {
    const resolved = resolveSecrets({ DB_HOST: '/var/run/postgresql', LOG_LEVEL: 'info' })
    expect(resolved.DB_HOST).toBe('/var/run/postgresql')
    expect(resolved.LOG_LEVEL).toBe('info')
  })

  test('a secret nobody configured stays undefined', () => {
    expect(resolveSecrets({ LOG_LEVEL: 'info' }).DB_PASSWORD).toBeUndefined()
  })
})
