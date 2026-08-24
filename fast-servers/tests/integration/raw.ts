/**
 * A socket that speaks bytes, not HTTP.
 *
 * `fetch` would test bun's idea of HTTP. What is interesting here is what happens to the bytes:
 * a request arriving one character at a time, two requests in one packet, a body that lands
 * after its head, and the things picohttpparser hands back as -1 or -2 rather than as a request.
 * None of that is expressible through a client library, because a client library exists to stop
 * you expressing it.
 */
import { Socket } from 'node:net'

export class Raw {
  private readonly socket: Socket
  private buffer: Buffer = Buffer.alloc(0)
  private ended = false
  private failure: Error | null = null
  private waiters: Array<() => void> = []

  private constructor(socket: Socket) {
    this.socket = socket
    socket.on('data', (chunk: Buffer) => {
      this.buffer = Buffer.concat([this.buffer, chunk])
      this.wake()
    })
    socket.on('end', () => {
      this.ended = true
      this.wake()
    })
    socket.on('close', () => {
      this.ended = true
      this.wake()
    })
    socket.on('error', (error: Error) => {
      // A reset is an outcome here, not a crash: several of these tests are about what the
      // server does to a connection, and the client finds out by being reset.
      this.failure = error
      this.ended = true
      this.wake()
    })
  }

  static connect(port: number, timeoutMs = 3000): Promise<Raw> {
    return new Promise((resolve, reject) => {
      const socket = new Socket()
      const timer = setTimeout(() => {
        socket.destroy()
        reject(new Error(`connect to ${port} timed out`))
      }, timeoutMs)
      socket.once('error', (error) => {
        clearTimeout(timer)
        reject(error)
      })
      socket.connect(port, '127.0.0.1', () => {
        clearTimeout(timer)
        socket.removeAllListeners('error')
        resolve(new Raw(socket))
      })
    })
  }

  private wake(): void {
    const waiting = this.waiters
    this.waiters = []
    for (const resolve of waiting) {
      resolve()
    }
  }

  /** Resolves on the next byte, on the close, or when @p ms have passed. */
  private settle(ms: number): Promise<void> {
    if (this.ended) {
      return Promise.resolve()
    }
    return new Promise((resolve) => {
      const timer = setTimeout(resolve, ms)
      this.waiters.push(() => {
        clearTimeout(timer)
        resolve()
      })
    })
  }

  /** Writes and does not wait. Errors surface on the next read, which is where they belong. */
  send(data: Buffer | string): void {
    if (this.socket.destroyed) {
      return
    }
    this.socket.write(typeof data === 'string' ? Buffer.from(data, 'latin1') : data)
  }

  /** Writes and waits for the kernel to take it, rejecting if the peer is gone. */
  sendChecked(data: Buffer | string): Promise<void> {
    return new Promise((resolve, reject) => {
      if (this.socket.destroyed) {
        reject(new Error('socket is closed'))
        return
      }
      this.socket.write(typeof data === 'string' ? Buffer.from(data, 'latin1') : data, (error) =>
        error ? reject(error) : resolve(),
      )
    })
  }

  /**
   * Writes @p chunk until the peer stops accepting, or until one of the bounds is reached.
   *
   * The bounds are not decoration. A write into a socket whose peer has answered and closed
   * keeps succeeding until the kernel buffer fills and the reset comes back, and how long that
   * takes is not something a test can depend on -- without the deadline this hangs for as long
   * as the runner allows and then reports a timeout instead of a result.
   *
   * @return how many bytes went out before it stopped
   */
  async flood(chunk: Buffer, maxBytes: number, maxMs: number): Promise<number> {
    const deadline = Date.now() + maxMs
    let sent = 0
    while (sent < maxBytes && Date.now() < deadline && !this.closed) {
      const remaining = Math.max(1, deadline - Date.now())
      const accepted = await Promise.race([
        this.sendChecked(chunk).then(
          () => true,
          () => false,
        ),
        Bun.sleep(remaining).then(() => false),
      ])
      if (!accepted) {
        break
      }
      sent += chunk.length
    }
    return sent
  }

  /** Everything the peer sends until it closes. */
  async readAll(timeoutMs = 3000): Promise<Buffer> {
    const deadline = Date.now() + timeoutMs
    while (!this.ended && Date.now() < deadline) {
      await this.settle(Math.max(1, deadline - Date.now()))
    }
    return this.buffer
  }

  /**
   * Exactly one response — head, then Content-Length bytes — consumed from the buffer, so the
   * next call reads the next response rather than the same one again. That is what makes the
   * keep-alive and pipelining tests able to tell two answers apart.
   */
  async readOne(timeoutMs = 3000): Promise<Buffer> {
    const deadline = Date.now() + timeoutMs
    for (;;) {
      const split = this.buffer.indexOf('\r\n\r\n')
      if (split >= 0) {
        const total = split + 4 + contentLengthOf(this.buffer.subarray(0, split))
        if (this.buffer.length >= total) {
          const response = this.buffer.subarray(0, total)
          this.buffer = this.buffer.subarray(total)
          return response
        }
      }
      if (this.ended || Date.now() >= deadline) {
        return this.buffer
      }
      await this.settle(Math.max(1, deadline - Date.now()))
    }
  }

  /** Whether the peer has closed the connection. */
  get closed(): boolean {
    return this.ended
  }

  close(): void {
    this.socket.destroy()
  }
}

function contentLengthOf(head: Buffer): number {
  for (const line of head.toString('latin1').split('\r\n')) {
    const [name, value] = splitOnce(line, ':')
    if (name.toLowerCase() === 'content-length') {
      return Number.parseInt(value.trim(), 10) || 0
    }
  }
  return 0
}

function splitOnce(line: string, separator: string): [string, string] {
  const at = line.indexOf(separator)
  return at < 0 ? [line, ''] : [line.slice(0, at), line.slice(at + 1)]
}

export function statusOf(response: Buffer): number {
  const parts = response.subarray(0, 32).toString('latin1').split(' ')
  return parts.length > 1 ? (Number.parseInt(parts[1], 10) ?? -1) : -1
}

export function headOf(response: Buffer): string {
  const split = response.indexOf('\r\n\r\n')
  return (split < 0 ? response : response.subarray(0, split)).toString('latin1')
}

export function bodyOf(response: Buffer): Buffer {
  const split = response.indexOf('\r\n\r\n')
  return split < 0 ? Buffer.alloc(0) : response.subarray(split + 4)
}

/** How many responses are in this buffer. */
export function responseCount(response: Buffer): number {
  return response.toString('latin1').split('HTTP/1.1 ').length - 1
}

/** Wire form of a chunked body, terminator and optional trailer included. */
export function chunked(parts: Array<Buffer | string>, trailer = ''): Buffer {
  const pieces: Buffer[] = []
  for (const part of parts) {
    const payload = typeof part === 'string' ? Buffer.from(part, 'latin1') : part
    pieces.push(
      Buffer.from(`${payload.length.toString(16)}\r\n`, 'latin1'),
      payload,
      Buffer.from('\r\n', 'latin1'),
    )
  }
  pieces.push(Buffer.from(`0\r\n${trailer}\r\n`, 'latin1'))
  return Buffer.concat(pieces)
}

export function bytes(...values: number[]): Buffer {
  return Buffer.from(values)
}

/** 0..255, which is what makes a body carry NUL bytes and a stray CR LF. */
export function everyByte(repeat = 1): Buffer {
  const one = Buffer.from(Array.from({ length: 256 }, (_, i) => i))
  return Buffer.concat(Array.from({ length: repeat }, () => one))
}
