/**
 * The entry point. Loading the addon by a path relative to this file keeps the
 * caller's working directory out of it.
 */

const os = require('node:os')

const isBun = typeof process !== 'undefined' && 'bun' in process.versions
const isWindows = os.platform() === 'win32'

let nativeBinding
try {
  // On Windows, Bun resolves the Node-API from bun.exe, so it gets its own
  // addon. Everywhere else one file serves both runtimes.
  nativeBinding =
    isBun && isWindows
      ? require('./build/shared_native.bun.node')
      : require('./build/shared_native.node')
} catch (cause) {
  throw new Error('the native addon is not built - run `npm run build`', { cause })
}

module.exports = nativeBinding
