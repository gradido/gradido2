import * as v from 'valibot'

/**
 * `v.parse` for a value that holds a secret: throws the first message and nothing else, because
 * the issues of a ValiError carry the input that failed.
 */
export function parseSecret<Input, Output>(
  schema: v.GenericSchema<Input, Output>,
  input: unknown,
): Output {
  const result = v.safeParse(schema, input)
  if (!result.success) {
    throw new Error(result.issues[0].message)
  }
  return result.output
}

/**
 * The result of a query that may find one row and must not find two: the row, or undefined.
 *
 * For a row that is unique by contract and not by an index. The statement asks for two, so that
 * a second one is noticed here instead of one of them being picked.
 */
export function atMostOneRowSchema<Input, Output>(
  row: v.GenericSchema<Input, Output>,
  message: string,
) {
  return v.pipe(
    v.array(row),
    v.maxLength(1, message),
    v.transform((rows): Output | undefined => rows[0]),
  )
}
