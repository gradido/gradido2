import { readFileSync, writeFileSync } from 'node:fs'
import { resolve } from 'node:path'

/**
 * Where `setup` writes what it was told, and where `config/index.ts` reads it back.
 *
 * The working directory, because that is where dotenv looks — a server started with
 * `turbo @gradido/backend#start` runs in `packages/backend/`, and the `.env.dist` beside
 * this file's package is the template for exactly that file. It is `.gitignore`d: it holds a
 * database password and an SMTP password.
 */
export function envFilePath(): string {
  return resolve(process.cwd(), '.env')
}

/**
 * Writes @p values into the env file, keeping everything already in it.
 *
 * A variable that is already there is replaced where it stands, so the comments an operator
 * wrote around it stay attached to it; one that is not is appended under a heading. That is
 * what makes `setup` safe to run a second time: it is not a file generator, it is an editor
 * that only touches the lines it has an answer for.
 *
 * Returns the path it wrote, for the sentence that reports it.
 */
export function writeEnvFile(values: Readonly<Record<string, string>>): string {
  const path = envFilePath()
  const existing = readEnvFile(path)
  const written = new Set<string>()

  const lines = existing.map((line) => {
    const name = assignedName(line)
    if (name === undefined || !(name in values) || written.has(name)) {
      return line
    }
    written.add(name)
    return `${name}=${quote(name, values[name] ?? '')}`
  })

  const added = Object.entries(values).filter(([name]) => !written.has(name))
  if (added.length !== 0) {
    if (lines.length !== 0 && lines.at(-1) !== '') {
      lines.push('')
    }
    lines.push('# written by the setup command')
    for (const [name, value] of added) {
      lines.push(`${name}=${quote(name, value)}`)
    }
  }

  writeFileSync(path, `${lines.join('\n')}\n`, { encoding: 'utf8', mode: 0o600 })
  return path
}

function readEnvFile(path: string): string[] {
  try {
    return readFileSync(path, 'utf8').replace(/\n$/u, '').split('\n')
  } catch {
    /* No file yet, which is the ordinary case on a first setup. Anything else — a directory
       in the way, no permission — surfaces at the write below, where the message can say
       what was being attempted. */
    return []
  }
}

/** The variable a line assigns to, or nothing for a comment, a blank line or a continuation. */
function assignedName(line: string): string | undefined {
  return /^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=/u.exec(line)?.[1]
}

/**
 * The value in the form dotenv reads back unchanged.
 *
 * dotenv strips one pair of surrounding quotes and unescapes nothing inside them except
 * `\n` and `\r`, and only in the double-quoted form. So the choice is not a style: bare
 * loses a `#` and the spaces around the value, single quotes carry everything but a `'`,
 * and double quotes carry everything but a `"` and a literal `\n`. Bare is what `.env.dist`
 * looks like and is used wherever it is exact.
 *
 * A value that neither form carries — one with both kinds of quote in it — is refused by
 * name rather than written and silently read back as something else. A password is the value
 * this can happen to, and a password that is quietly wrong is a login failure nobody traces
 * back to this file.
 */
function quote(name: string, value: string): string {
  if (/^[A-Za-z0-9_@%+:,./=-]*$/u.test(value)) {
    return value
  }
  if (!value.includes("'")) {
    return `'${value}'`
  }
  if (!value.includes('"') && !/\\[nr]/u.test(value)) {
    return `"${value}"`
  }
  throw new Error(
    `${name} cannot be written to .env: dotenv reads no quoting that carries both a ' and a " in one value. Set it in the environment instead, or choose a value without one of them.`,
  )
}
