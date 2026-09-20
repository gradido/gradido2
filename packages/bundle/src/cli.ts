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
 * The entry point of the single binary: pick the services, hand them the rest of the command line.
 *
 * `gradido` alone is `gradido backend serve`, which is the download-and-start promise of
 * `Architecture.md` — one file, no arguments, a server. A named service takes the arguments
 * after its name, so `gradido backend setup` and `gradido setup` are the same command: the
 * service may be left out, and then it is the backend.
 *
 * Several services may be named, `gradido backend dht-node`, and then they share the process,
 * as `gradido2-fast --backend --dht-node` does on the fast path: the backend answers
 * `peer.bootstrap` from the node beside it. Several services only serve -- a command such as
 * `setup` belongs to one service and is refused after two.
 */
export async function runGradido(argv: readonly string[], binary: GradidoBinary): Promise<void> {
  if (!binary.sites.length || binary.sites[0].name !== 'frontend') {
    // biome-ignore lint/suspicious/noConsole: startup can fail before there is a logger
    console.error('Missing Frontend Page')
    return
  }
  const first = argv[0]

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

  let named = 0
  while (isService(argv[named])) {
    named++
  }
  const services = named === 0 ? (['backend'] as const) : (argv.slice(0, named) as Service[])
  const args = argv.slice(named)
  if (new Set(services).size !== services.length) {
    return refuse(`a service is named twice: ${services.join(' ')}`)
  }
  if (services.length > 1 && args.length !== 0) {
    return refuse(`"${args.join(' ')}" is a command of one service, not of ${services.join(' ')}`)
  }

  await Promise.all(services.map((service) => runService(service, args, binary)))
}

async function runService(
  service: Service,
  args: readonly string[],
  binary: GradidoBinary,
): Promise<void> {
  switch (service) {
    case 'backend':
      /* The pages go with the backend and with nothing else: they are what a browser asks
         this server for, and the federation server has no browser. */
      return await runBackend(args, { frontend: binary.sites[0] })

    case 'federation':
      /* `packages/federation` does not exist yet. When it does, this becomes
         `return await runFederation(args)` — same shape as the backend above, because it is
         the same kind of thing: an Elysia server over `service-core`, mounted at
         `/api/{apiVersion}` rather than at the root. It gets no sites. */
      return refuse('"federation" is not implemented yet — it will live in packages/federation.')

    case 'dht-node': {
      /* Imported here and not at the top: its configuration is checked when the module loads,
         and a backend start must not fail over a dht setting it never reads. The bundler still
         embeds it -- a static specifier in an import() is followed like any other. */
      const { runDhtNode } = await import('@gradido/dht-node/main')
      return await runDhtNode(args)
    }
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

/** An argument this binary cannot act on. Refuses rather than starting something else. */
function refuse(message: string): never {
  // biome-ignore lint/suspicious/noConsole: an unusable argument, before anything is open
  console.error(message)
  process.exit(1)
}

function usage(): string {
  return `gradido2 — the Gradido server: every service, and the pages, in one file

usage:  gradido2 [service...] [command]
        gradido2 backend dht-node     both, in one process; a command takes one service

services
  backend       the HTTP API, and the frontends this binary carries (the default)
  federation    not built yet
  dht-node      the peer network node: finds communities, carries their calls, relays

backend commands
  serve         start the server (the default)
  setup         say who this community is, then stop. Run once, with a
                terminal attached, before the first serve
  migrate-down  take the database down one migration, then stop.
                Needs DB_MIGRATE_DOWN to name the migration to end at

options
  -h, --help     this text
  -v, --version  the version this binary was built from

Everything else is configuration, and configuration is the environment — read from a
\`.env\` next to the binary if there is one. \`packages/backend/.env.dist\` lists what
there is; nothing in it has to be set for the server to start.`
}
