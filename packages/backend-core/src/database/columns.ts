import { getColumns, type Table } from 'drizzle-orm'

/**
 * The columns of @p table that @p schema reads, for a `select(…)`: the schema is the column
 * list, so a column added to it is selected in both dialects without being named again.
 *
 * For a row that must leave something out -- a secret, a large column. A row that is the whole
 * table needs no list and takes a plain `select()`.
 */
export function columnsFor<Of extends Table, Key extends keyof Of['_']['columns']>(
  table: Of,
  schema: { readonly entries: Record<Key, unknown> },
): Pick<Of['_']['columns'], Key> {
  const columns: Of['_']['columns'] = getColumns(table)
  const selected = {} as Pick<Of['_']['columns'], Key>
  for (const key of Object.keys(schema.entries) as Key[]) {
    selected[key] = columns[key]
  }
  return selected
}
