import { runDhtNode } from './main'

/**
 * The dht-node role as its own process: `bun src/index.ts`. Everything it does is in `main.ts`,
 * because the single binary starts the same role without being this file.
 */
runDhtNode(process.argv.slice(2)).catch((error) => {
  // biome-ignore lint/suspicious/noConsole: startup can fail before there is a logger
  console.error(error)
  process.exit(1)
})
