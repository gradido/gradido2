import { afterEach, beforeEach, describe, expect, test } from 'bun:test'
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { envFilePath, writeEnvFile } from './envFile'

/* The file is found through the working directory, so a test that writes one moves there. */
let directory = ''
let previous = ''

beforeEach(() => {
  previous = process.cwd()
  directory = mkdtempSync(join(tmpdir(), 'gradido-env-'))
  process.chdir(directory)
})

afterEach(() => {
  process.chdir(previous)
  rmSync(directory, { recursive: true, force: true })
})

const written = (): string => readFileSync(envFilePath(), 'utf8')

describe('writeEnvFile', () => {
  test('writes a file that was not there', () => {
    writeEnvFile({ DB_TYPE: 'sqlite', EMAIL: 'false' })

    expect(written()).toBe('# written by the setup command\nDB_TYPE=sqlite\nEMAIL=false\n')
  })

  test('replaces a variable where it stands, keeping the comment above it', () => {
    writeFileSync(envFilePath(), '# the database\nDB_TYPE=sqlite\nBACKEND_PORT=4000\n')
    writeEnvFile({ DB_TYPE: 'postgresql' })

    expect(written()).toBe('# the database\nDB_TYPE=postgresql\nBACKEND_PORT=4000\n')
  })

  test('leaves every variable it was given no answer for', () => {
    writeFileSync(envFilePath(), 'LOG_LEVEL=debug\nBACKEND_PORT=4001\n')
    writeEnvFile({ EMAIL: 'true' })

    expect(written()).toContain('LOG_LEVEL=debug')
    expect(written()).toContain('BACKEND_PORT=4001')
    expect(written()).toContain('EMAIL=true')
  })

  test('a commented-out variable is not the one that gets the answer', () => {
    writeFileSync(envFilePath(), '# EMAIL=true\n')
    writeEnvFile({ EMAIL: 'false' })

    expect(written()).toBe('# EMAIL=true\n\n# written by the setup command\nEMAIL=false\n')
  })

  test('quotes a value the unquoted form would change', () => {
    writeEnvFile({ EMAIL_PASSWORD: 'a # b "c"', EMAIL_SENDER: 'dev@gradido.localhost' })

    expect(written()).toContain(`EMAIL_PASSWORD='a # b "c"'`)
    /* An ordinary value stays bare, because that is what every line of .env.dist looks like. */
    expect(written()).toContain('EMAIL_SENDER=dev@gradido.localhost')
  })

  test('refuses a value no quoting carries rather than writing a different one', () => {
    expect(() => writeEnvFile({ EMAIL_PASSWORD: `both ' and "` })).toThrow('EMAIL_PASSWORD')
  })

  test.each([
    ['a # b'],
    ['  padded  '],
    [String.raw`back\slash`],
    [String.raw`literal \n, not a newline`],
    ['a "quoted" word'],
    ["it's mine"],
    ['line one\nline two'],
    [''],
    ['plain'],
  ])('reads back through dotenv as what was written: %j', async (secret: string) => {
    writeEnvFile({ EMAIL_PASSWORD: secret })

    const { parse } = await import('dotenv')
    expect(parse(written()).EMAIL_PASSWORD).toBe(secret)
  })
})
