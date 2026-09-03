import { runBackend } from '@gradido/backend/main'
import type { StaticSite } from '@gradido/backend/server'

/**
 * Everything the executable was built with that the code inside it cannot work out for
 * itself: which pages it carries, and which version it is.
 *
 * It is handed in rather than read, because both answers are made by the build: they arrive
 * from `gen/main.ts`, the entry point `scripts/bundle.ts` writes, whose header explains what
 * is in it and why it is generated.
 */
export type GradidoBinary = {
  /** The version of the repository this was built from. */
  readonly version: string
  /** The frontends compiled into the binary, in the order they should be matched. */
  readonly sites: readonly StaticSite[]
}

/**
 * The services one binary can start. `backend` is the default, because a Gradido server is
 * what somebody who downloaded this wanted; the others are a deployment deciding to run a
 * process for one job.
 */
const SERVICES = ['backend', 'federation', 'dht-node'] as const

type Service = (typeof SERVICES)[number]

const isService = (value: string | undefined): value is Service =>
  (SERVICES as readonly string[]).includes(value ?? '')

/**
 * The entry point of the single binary: pick a service, hand it the rest of the command line.
 *
 * `gradido` alone is `gradido backend serve`, which is the download-and-start promise of
 * `Architecture.md` — one file, no arguments, a server. A named service takes the arguments
 * after its name, so `gradido backend migrate-down` and `gradido migrate-down` are the same
 * command: the service may be left out, and then it is the backend.
 *
 * Nothing here starts more than one service. Two of them in one process would share a heap
 * and a signal handler and would be a deployment decision made by an argument parser — a
 * deployment that wants a federation server next to a backend starts the binary twice, which
 * is also how it gets to put them on different machines.
 */
export async function runGradido(argv: readonly string[], binary: GradidoBinary): Promise<void> {
  const [first, ...rest] = argv

  if (first === '--help' || first === '-h') {
    // biome-ignore lint/suspicious/noConsole: this is the output somebody asked for
    console.log(usage())
    return
  }
  if (first === '--version' || first === '-v') {
    // biome-ignore lint/suspicious/noConsole: this is the output somebody asked for
    console.log(binary.version)
    return
  }

  const service: Service = isService(first) ? first : 'backend'
  const args = isService(first) ? rest : argv

  switch (service) {
    case 'backend':
      /* The pages go with the backend and with nothing else: they are what a browser asks
         this server for, and the federation server has no browser. */
      return await runBackend(args, { sites: binary.sites })

    case 'federation':
      /* `packages/federation` does not exist yet. When it does, this becomes
         `return await runFederation(args)` — same shape as the backend above, because it is
         the same kind of thing: an Elysia server over `service-core`, mounted at
         `/api/{apiVersion}` rather than at the root. It gets no sites. */
      return unavailable(service, 'packages/federation')

    case 'dht-node':
      /* `packages/dht-node` does not exist yet either, and it will not look like the two
         above: js-libp2p, no HTTP server of its own, no database — see Architecture.md,
         *Peer discovery*. It still starts from here, because it is a service a deployment
         runs as a process, and this binary is how a process is started. */
      return unavailable(service, 'packages/dht-node')
  }
}

/**
 * What the generated entry point calls.
 *
 * The entry is written by `scripts/bundle.ts` and is nothing but the embedded files and this
 * call, so what happens to an error that reaches the top belongs here, where it can be read,
 * rather than in generated code.
 */
export function startGradido(argv: readonly string[], binary: GradidoBinary): void {
  runGradido(argv, binary).catch((error) => {
    // biome-ignore lint/suspicious/noConsole: startup can fail before there is a logger
    console.error(error)
    process.exit(1)
  })
}

/** A service this build has no implementation for. Refuses rather than starting something else. */
function unavailable(service: Service, where: string): never {
  // biome-ignore lint/suspicious/noConsole: an unusable argument, before anything is open
  console.error(`"${service}" is not implemented yet — it will live in ${where}.`)
  process.exit(1)
}

function usage(): string {
  return `gradido2 — the Gradido server: every service, and the pages, in one file

usage:  gradido2 [service] [command]

services
  backend       the HTTP API, and the frontends this binary carries (the default)
  federation    not built yet
  dht-node      not built yet

backend commands
  serve         start the server (the default)
  migrate-down  take the database down one migration, then stop.
                Needs DB_MIGRATE_DOWN to name the migration to end at

options
  -h, --help     this text
  -v, --version  the version this binary was built from

Everything else is configuration, and configuration is the environment — read from a
\`.env\` next to the binary if there is one. \`packages/backend/.env.dist\` lists what
there is; nothing in it has to be set for the server to start.`
}
