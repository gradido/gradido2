import { describe, expect, test } from 'bun:test'
import { createInsertSchema, createSelectSchema } from 'drizzle-orm/valibot'
import * as v from 'valibot'
import { communitiesPg, communitiesSqlite } from '../../database'
import {
  homeCommunityInsertSchema,
  homeCommunitySchema,
  homeCommunitySelectSchema,
  homeCommunitySigningKeySelectSchema,
} from './community.schema'

/**
 * The schemas written by hand against the ones drizzle derives from the table definitions:
 * the same keys, and values the other side takes. A column renamed or retyped in
 * `tables/communities.ts` and not here fails in this file rather than at the first start.
 */
const communityUuid = '5f2c1d0e-8a4b-4c3d-9e6f-0a1b2c3d4e5f'
const publicKey = Buffer.alloc(32, 1)
const privateKey = Buffer.alloc(64, 2)
const createdAt = new Date('2026-10-01T12:00:00.000Z')

/** A whole row, as a `select()` on the table hands it out. Only the id differs by dialect. */
const row = {
  remote: false,
  url: 'https://gdd.example.org',
  publicKey,
  privateKey,
  communityUuid,
  name: 'Gradido Entwicklung',
  description: null,
  creationDate: createdAt,
  createdAt,
  updatedAt: null,
}

const dialects = [
  { kind: 'postgresql', table: communitiesPg, row: { id: 1n, ...row } },
  { kind: 'sqlite', table: communitiesSqlite, row: { id: 1, ...row } },
] as const

const homeCommunityEntries: Record<string, v.GenericSchema> = homeCommunitySchema.entries

for (const dialect of dialects) {
  describe(`the community schemas against the ${dialect.kind} table`, () => {
    const select = createSelectSchema(dialect.table)
    const insert = createInsertSchema(dialect.table)
    const selectEntries: Record<string, v.GenericSchema> = select.entries
    const insertEntries: Record<string, v.GenericSchema> = insert.entries

    test('the sample row is one drizzle would hand out', () => {
      expect(v.safeParse(select, dialect.row).issues).toBeUndefined()
    })

    test('homeCommunitySchema reads only columns that exist', () => {
      expect(Object.keys(selectEntries)).toEqual(
        expect.arrayContaining(Object.keys(homeCommunityEntries)),
      )
    })

    test('homeCommunitySchema leaves the private key out', () => {
      expect(Object.keys(homeCommunityEntries)).not.toContain('privateKey')
    })

    test('homeCommunitySchema takes null wherever the column is nullable', () => {
      for (const [key, entry] of Object.entries(homeCommunityEntries)) {
        if (selectEntries[key].type === 'nullable') {
          expect(`${key} ${v.safeParse(entry, null).success}`).toBe(`${key} true`)
        }
      }
    })

    test('homeCommunitySelectSchema reads the row drizzle hands out', () => {
      expect(v.parse(homeCommunitySelectSchema, [dialect.row])).toEqual({
        id: 1n,
        communityUuid,
        url: row.url,
        name: row.name,
        description: null,
        publicKey: new Uint8Array(publicKey),
      })
    })

    test('homeCommunitySigningKeySelectSchema reads the column drizzle hands out', () => {
      expect(Object.keys(selectEntries)).toContain('privateKey')
      expect(
        v.parse(homeCommunitySigningKeySelectSchema, [{ privateKey: dialect.row.privateKey }]),
      ).toEqual(new Uint8Array(privateKey))
    })

    describe('homeCommunityInsertSchema', () => {
      const written = v.parse(homeCommunityInsertSchema, {
        url: row.url,
        name: row.name,
        description: null,
        communityUuid,
        publicKey,
        privateKey,
        createdAt,
      })

      test('writes only columns that exist', () => {
        expect(Object.keys(insertEntries)).toEqual(expect.arrayContaining(Object.keys(written)))
      })

      test('writes values the table takes, with no required column missing', () => {
        expect(v.safeParse(insert, written).issues).toBeUndefined()
      })

      test('leaves out only what the statement or the database fills in', () => {
        const left = Object.keys(insertEntries).filter((key) => !(key in written))
        // `id` is generated and absent from PostgreSQL's insert schema altogether.
        expect(left.filter((key) => key !== 'id').sort()).toEqual(['remote', 'updatedAt'])
      })
    })
  })
}
