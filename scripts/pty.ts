/**
 * A command on a pseudo-terminal, answered like somebody sitting at it: for end-to-end tests of
 * `setup`, which refuses to ask anything without a terminal. Bun has no pty of its own, so
 * Python's `pty.spawn` relays -- present on every CI image and every Linux or macOS machine.
 *
 * Whenever the program has printed something and then gone quiet, it is waiting for an answer:
 * `answer` is handed what appeared since the last one and says what to type. Enter, when it says
 * nothing -- every question of `setup` has a default.
 */
export interface Driven {
  readonly exitCode: number
  readonly output: string
}

export interface DriveOptions {
  readonly cwd: string
  readonly env: Record<string, string>
  readonly answer?: (screen: string) => string | undefined
  /** How long the program must be quiet before it counts as asking. */
  readonly quietMs?: number
  readonly timeoutMs?: number
}

const RELAY = 'import os, pty, sys; sys.exit(os.waitstatus_to_exitcode(pty.spawn(sys.argv[1:])))'

export async function drive(command: readonly string[], options: DriveOptions): Promise<Driven> {
  const child = Bun.spawn(['python3', '-c', RELAY, ...command], {
    cwd: options.cwd,
    env: options.env,
    stdin: 'pipe',
    stdout: 'pipe',
    stderr: 'pipe',
  })
  const quietMs = options.quietMs ?? 400
  const deadline = Date.now() + (options.timeoutMs ?? 60_000)
  const decoder = new TextDecoder()
  let output = ''
  let screen = ''
  let lastOutput = Date.now()
  let done = false

  const reading = (async () => {
    for await (const chunk of child.stdout) {
      const text = decoder.decode(chunk, { stream: true })
      output += text
      screen += text
      lastOutput = Date.now()
    }
  })()
  const exited = child.exited.then((code) => {
    done = true
    return code
  })

  while (!done) {
    if (Date.now() > deadline) {
      child.kill()
      throw new Error(`${command.join(' ')} did not finish; last screen:\n${screen}`)
    }
    await Bun.sleep(50)
    if (screen !== '' && Date.now() - lastOutput >= quietMs && !done) {
      const typed = options.answer?.(screen) ?? '\r'
      screen = ''
      child.stdin.write(typed)
      child.stdin.flush()
    }
  }
  const exitCode = await exited
  await reading
  return { exitCode, output: output + (await new Response(child.stderr).text()) }
}
