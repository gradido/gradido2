/**
 * Starts the probe server and waits for it to say it is listening.
 *
 * Which binary is up to the caller: the same suite runs against the h2o build and the fallback
 * build, and the whole point is that both answer the same. `FS_HTTP_PROBE` names it.
 */

import type { ChildProcessWithoutNullStreams } from 'node:child_process'
import { spawn } from 'node:child_process'
import { Raw } from './raw'

export interface Probe {
  readonly port: number
  readonly backend: string
  stop(): Promise<void>
}

const DEFAULT_BINARY = new URL('../../zig-out/bin/http-probe', import.meta.url).pathname

export async function startProbe(): Promise<Probe> {
  const binary = process.env.FS_HTTP_PROBE ?? DEFAULT_BINARY
  const port = Number.parseInt(process.env.FS_HTTP_PROBE_PORT ?? '17899', 10)

  const child: ChildProcessWithoutNullStreams = spawn(binary, [String(port)], {
    stdio: ['ignore', 'pipe', 'pipe'],
  })

  let stderr = ''
  child.stderr.on('data', (chunk: Buffer) => {
    stderr += chunk.toString()
  })

  // A probe that dies mid-run turns every later test into ECONNREFUSED, which says nothing
  // about why. Report the exit once, with whatever it managed to write.
  let stopping = false
  child.once('exit', (code, signal) => {
    if (!stopping) {
      // A probe that dies turns every later test into ECONNREFUSED, which says nothing about
      // why. This is the only place that can.
      // biome-ignore lint/suspicious/noConsole: the harness reports, it does not log
      console.error(`http-probe exited unexpectedly: code=${code} signal=${signal}\n${stderr}`)
    }
  })

  // The probe prints one line on stdout when it is listening. Waiting for that rather than
  // polling the port distinguishes "not up yet" from "refused to start", which otherwise look
  // the same for three seconds and then produce the same unhelpful timeout.
  const announcement = await new Promise<string>((resolve, reject) => {
    let out = ''
    const timer = setTimeout(
      () => reject(new Error(`${binary} did not start within 5s\n${stderr}`)),
      5000,
    )
    child.stdout.on('data', (chunk: Buffer) => {
      out += chunk.toString()
      if (out.includes('\n')) {
        clearTimeout(timer)
        resolve(out)
      }
    })
    child.once('exit', (code) => {
      clearTimeout(timer)
      reject(new Error(`${binary} exited with ${code}\n${stderr}`))
    })
  })

  const backend = /backend=(\S+)/.exec(announcement)?.[1] ?? 'unknown'

  // Listening is announced before the loop runs; one accepted connection proves the loop is
  // actually turning.
  for (let attempt = 0; attempt < 50; ++attempt) {
    try {
      ;(await Raw.connect(port, 200)).close()
      break
    } catch {
      await Bun.sleep(20)
    }
  }

  return {
    port,
    backend,
    async stop() {
      stopping = true
      child.kill('SIGINT')
      await new Promise<void>((resolve) => {
        const timer = setTimeout(() => {
          child.kill('SIGKILL')
          resolve()
        }, 3000)
        child.once('exit', () => {
          clearTimeout(timer)
          resolve()
        })
      })
    },
  }
}
