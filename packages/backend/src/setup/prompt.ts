import { createInterface } from 'node:readline/promises'
import * as v from 'valibot'

/**
 * The terminal half of the `setup` command: one question at a time, on stdin and stdout.
 *
 * Three shapes of question and nothing else — pick one of a few, type a value, type a value
 * nobody should see. There is no prompt library behind them: `AGENTS.md` section 14 asks
 * what a dependency is worth, and a dependency that runs once per installation, in a command
 * that is already the only place in the codebase reading a terminal, is not worth a
 * lockfile entry — and the counterpart in `fast-servers/backend/src/prompt.c` has to be
 * written by hand either way, so this is the shape both of them keep.
 *
 * **Every question has a default and Enter takes it.** A setup that can be finished by
 * holding Enter down is a setup somebody will actually run; the defaults are what the
 * conversation in `askForSetup.ts` proposes, and what it proposes is what the environment
 * already says.
 */

/** Whether there is somebody at the other end to answer. */
export function isTerminal(): boolean {
  return process.stdin.isTTY === true && process.stdout.isTTY === true
}

export type Choice<T> = {
  readonly value: T
  readonly label: string
  /** One clause after the label, saying what choosing it means. */
  readonly hint?: string
}

/**
 * One of a handful, chosen with the arrow keys and Enter.
 *
 * Raw mode is what makes a cursor possible, and a terminal that will not give it — a pipe,
 * a CI job, a Windows console this build cannot switch — gets the same question as a
 * numbered list to type an answer to. The choice is the same either way; only the typing is.
 */
export async function askChoice<T>(
  question: string,
  choices: readonly Choice<T>[],
  initial = 0,
): Promise<T> {
  process.stdout.write(`\n${question}\n`)
  const stdin = process.stdin
  if (!isTerminal() || typeof stdin.setRawMode !== 'function') {
    return askNumbered(choices, initial)
  }

  return new Promise<T>((resolve) => {
    let index = initial
    stdin.setRawMode(true)
    stdin.resume()
    render(choices, index)

    let answered = false

    const finish = (chosen: number): void => {
      answered = true
      stdin.off('data', onData)
      stdin.setRawMode(false)
      stdin.pause()
      /* The list collapses to the answer, so that a finished setup reads back as questions
         and answers rather than as a screenful of menus. */
      process.stdout.write(`\u001b[${choices.length}A\u001b[J❯ ${choices[chosen]?.label ?? ''}\n`)
      resolve(choices[chosen]?.value as T)
    }

    const onData = (chunk: Buffer): void => {
      for (const key of keysIn(chunk)) {
        if (answered) {
          return
        }
        if (key === '\u0003') {
          /* Ctrl-C, which raw mode does not turn into a signal. 130 is what a shell reports
             for a program that ended on SIGINT, and setup has written nothing at this point. */
          stdin.setRawMode(false)
          process.stdout.write('\n')
          process.exit(130)
        }
        if (key === '\r' || key === '\n') {
          finish(index)
          continue
        }
        const digit = Number.parseInt(key, 10)
        if (Number.isInteger(digit) && digit >= 1 && digit <= choices.length) {
          finish(digit - 1)
          continue
        }
        const moved = step(key)
        if (moved !== 0) {
          index = (index + moved + choices.length) % choices.length
          process.stdout.write(`\u001b[${choices.length}A`)
          render(choices, index)
        }
      }
    }

    stdin.on('data', onData)
  })
}

/** Yes or no, as a choice — the same cursor, and Enter on what is already proposed. */
export async function askYesNo(question: string, initial: boolean): Promise<boolean> {
  return askChoice<boolean>(
    question,
    [
      { value: true, label: 'yes' },
      { value: false, label: 'no' },
    ],
    initial ? 0 : 1,
  )
}

/**
 * One typed value, checked against the same schema the rest of the system checks it with.
 *
 * An empty line takes `fallback`, which is what the parentheses after the label show. A
 * rejected answer is asked again rather than ending the setup: somebody is standing at the
 * terminal, and losing eleven correct answers because the twelfth had a typo would be
 * gratuitous.
 */
export async function askText<TSchema extends v.GenericSchema<string, unknown>>(
  label: string,
  fallback: string,
  schema: TSchema,
): Promise<v.InferOutput<TSchema>> {
  for (;;) {
    const typed = await readLine(fallback === '' ? `${label}: ` : `${label} (${fallback}): `)
    const answer = v.safeParse(schema, typed === '' ? fallback : typed)
    if (answer.success) {
      return answer.output
    }
    /* The schema's own message, which is the one the admin frontend will show for the same
       field later. Written to stderr so a piped stdout stays clean. */
    process.stderr.write(`  ${answer.issues[0].message}\n`)
  }
}

/**
 * A value that must not end up in the scrollback of a shared terminal.
 *
 * Echoed as asterisks, which needs raw mode; without it the answer is read the ordinary way
 * and is visible, because a setup that cannot ask for a password at all is worse than one
 * that asks for it in the open. An empty line keeps whatever is configured now, so a rerun
 * of `setup` does not have to retype it.
 */
export async function askSecret(label: string, fallback: string): Promise<string> {
  const shown = fallback === '' ? 'none' : 'unchanged'
  const stdin = process.stdin
  if (!isTerminal() || typeof stdin.setRawMode !== 'function') {
    const typed = await readLine(`${label} (${shown}): `)
    return typed === '' ? fallback : typed
  }

  process.stdout.write(`${label} (${shown}): `)
  return new Promise<string>((resolve) => {
    let typed = ''
    stdin.setRawMode(true)
    stdin.resume()

    let answered = false

    const onData = (chunk: Buffer): void => {
      for (const key of keysIn(chunk)) {
        if (answered) {
          return
        }
        if (key === '\u0003') {
          stdin.setRawMode(false)
          process.stdout.write('\n')
          process.exit(130)
        }
        if (key === '\r' || key === '\n') {
          answered = true
          stdin.off('data', onData)
          stdin.setRawMode(false)
          stdin.pause()
          process.stdout.write('\n')
          resolve(typed === '' ? fallback : typed)
          return
        }
        if (key === '\u007f' || key === '\b') {
          if (typed !== '') {
            typed = typed.slice(0, -1)
            process.stdout.write('\b \b')
          }
          continue
        }
        /* Everything else is text, except the escape sequences an arrow key sends — there is
           nothing to move a cursor over here, and letting them through would put a `[D` in a
           password. */
        if (key.startsWith('\u001b')) {
          continue
        }
        typed += key
        process.stdout.write('*')
      }
    }

    stdin.on('data', onData)
  })
}

/** A sentence that is part of the conversation rather than part of the log. */
export function say(...lines: readonly string[]): void {
  process.stdout.write(`${lines.join('\n')}\n`)
}

async function readLine(query: string): Promise<string> {
  /* One interface per question, because `askChoice` and `askSecret` read the same stdin
     directly and two readers on one stream take a keystroke each. */
  const io = createInterface({ input: process.stdin, output: process.stdout })
  try {
    return (await io.question(query)).trim()
  } finally {
    io.close()
  }
}

/**
 * One chunk from a terminal, as the keys that are in it.
 *
 * A keystroke is usually a chunk, and is not always: a paste arrives as one, and so does an
 * arrow key pressed a moment before Enter — the terminal hands over whatever has accumulated.
 * A handler that compared the whole chunk to `\r` would ignore both.
 */
function keysIn(chunk: Buffer): string[] {
  const text = chunk.toString('utf8')
  const keys: string[] = []
  for (let at = 0; at < text.length; ) {
    /* A CSI sequence — what every arrow key sends — is three characters and one key. */
    const csi = text.startsWith('\u001b[', at)
    keys.push(text.slice(at, at + (csi ? 3 : 1)))
    at += csi ? 3 : 1
  }
  return keys
}

/** Which way an arrow key points, or 0 for a key that is not one. */
function step(key: string): number {
  if (key === '\u001b[A' || key === '\u001b[D') {
    return -1
  }
  if (key === '\u001b[B' || key === '\u001b[C') {
    return 1
  }
  return 0
}

function render<T>(choices: readonly Choice<T>[], index: number): void {
  for (const [at, choice] of choices.entries()) {
    const chosen = at === index
    const label = chosen ? `\u001b[1m${choice.label}\u001b[22m` : choice.label
    const hint = choice.hint === undefined ? '' : `  — ${choice.hint}`
    /* Cleared line by line rather than as a block: the line before may have been longer, and
       half a hint left standing under a shorter one reads as part of it. */
    process.stdout.write(`\u001b[2K${chosen ? '❯ ' : '  '}${label}${hint}\n`)
  }
}

async function askNumbered<T>(choices: readonly Choice<T>[], initial: number): Promise<T> {
  for (const [at, choice] of choices.entries()) {
    const hint = choice.hint === undefined ? '' : ` — ${choice.hint}`
    process.stdout.write(`  ${at + 1}) ${choice.label}${hint}\n`)
  }
  for (;;) {
    const typed = await readLine(`Choose 1-${choices.length} (${initial + 1}): `)
    if (typed === '') {
      return choices[initial]?.value as T
    }
    const chosen = choices.findIndex(
      (choice, at) =>
        typed === String(at + 1) || typed.toLowerCase() === choice.label.toLowerCase(),
    )
    if (chosen !== -1) {
      return choices[chosen]?.value as T
    }
    process.stderr.write(`  Please answer with a number between 1 and ${choices.length}\n`)
  }
}
