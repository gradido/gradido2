import { readFileSync, writeFileSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import * as v from 'valibot'

/**
 * Loading a file out of `contracts/test-vectors/`, the half of it that is the same for every
 * subject.
 *
 * A contract vector file is read by two runners: this one, and a C one in
 * `fast-servers/tests/contract/`. Neither owns the file and neither may skip a vector it does
 * not like — a vector nobody runs is a disagreement nobody reports, which is the failure this
 * whole arrangement exists to prevent. So the envelope is checked here rather than trusted:
 * the declared `count` has to be the number of vectors that were actually read, and every id
 * has to be unique and to name its subject. The C loader checks the same three things, for the
 * same reason.
 *
 * What is *in* a vector is the subject's business. Pass the schema for it; this file has no
 * opinion beyond the envelope.
 *
 * Read `contracts/AGENTS.md`, *test-vectors*, before adding a subject.
 */

/** `contracts/test-vectors/`, found from this file rather than from the working directory. */
export function testVectorsDir(): string {
  return join(
    dirname(fileURLToPath(import.meta.url)),
    '..',
    '..',
    '..',
    'contracts',
    'test-vectors',
  )
}

/** The path of one subject's vector file. */
export function vectorFilePath(subject: string): string {
  return join(testVectorsDir(), `${subject}.json`)
}

/**
 * A typed value as `contracts/` spells one: the value is always text, the type is beside it.
 *
 * Numbers are decimal strings here because a JSON number above 2^53 does not survive a
 * JavaScript parser and a C one disagrees about integer-versus-double. `contracts/AGENTS.md`
 * has the rule.
 */
export const contractValueSchema = v.object({
  type: v.string(),
  value: v.string(),
  unit: v.optional(v.string()),
  note: v.optional(v.string()),
})

export type ContractValue = v.InferOutput<typeof contractValueSchema>

/** The envelope every vector file carries, whatever its subject. */
const envelopeSchema = v.object({
  contractVersion: v.literal(1),
  kind: v.literal('test-vectors'),
  subject: v.string(),
  notes: v.array(v.string()),
  fields: v.record(v.string(), v.unknown()),
  count: v.number(),
  vectors: v.array(v.unknown()),
})

/**
 * Reads `contracts/test-vectors/<subject>.json` and returns its vectors, parsed by @p vectorSchema.
 *
 * Throws rather than returns on anything wrong with the file. A malformed contract is not a
 * test failure to be counted, it is a suite that cannot say anything at all.
 */
export function loadVectors<TSchema extends v.GenericSchema>(
  subject: string,
  vectorSchema: TSchema,
): { readonly file: Record<string, unknown>; readonly vectors: v.InferOutput<TSchema>[] } {
  const path = vectorFilePath(subject)
  const raw: unknown = JSON.parse(readFileSync(path, 'utf8'))
  const envelope = v.parse(envelopeSchema, raw)

  if (envelope.subject !== subject) {
    throw new Error(`${path}: subject is "${envelope.subject}", expected "${subject}"`)
  }
  if (envelope.count !== envelope.vectors.length) {
    throw new Error(
      `${path}: declares count ${envelope.count} and carries ${envelope.vectors.length} vectors`,
    )
  }
  if (envelope.count === 0) {
    throw new Error(`${path}: a subject with no vectors passes by saying nothing`)
  }

  const vectors = envelope.vectors.map((vector, index) => {
    const parsed = v.safeParse(vectorSchema, vector)
    if (!parsed.success) {
      const id = (vector as { id?: unknown }).id ?? `#${index}`
      throw new Error(`${path}: vector ${String(id)}: ${parsed.issues[0]?.message}`)
    }
    return parsed.output
  })

  const ids = new Set<string>()
  for (const vector of vectors) {
    const { id } = vector as { id: string }
    if (!id.startsWith(`${subject}.`)) {
      throw new Error(`${path}: id "${id}" does not name its subject`)
    }
    if (ids.has(id)) {
      throw new Error(`${path}: id "${id}" appears twice; an id names one vector forever`)
    }
    ids.add(id)
  }

  return { file: raw as Record<string, unknown>, vectors }
}

/**
 * Writes a vector file back, for a regenerator that recomputed a derived field.
 *
 * Two spaces and a trailing newline, which is what biome formats `contracts/**` to — a
 * regenerator whose output the formatter then rewrites would show up as a diff in every commit
 * that touched it.
 */
export function writeVectorFile(subject: string, file: unknown): void {
  writeFileSync(vectorFilePath(subject), `${JSON.stringify(file, null, 2)}\n`)
}
