import { z, ZodSchema } from 'zod'
export * from './commonSchema'

export function validate(schema: ZodSchema, data: unknown) {
  const result = schema.safeParse(data)
  if (!result.success) {
    for (const issue of result.error.issues) {
      const key = issue.path.join('.')
      const value = (data as any)[key]
      const details = JSON.stringify(issue, null, 2)
      if (value === undefined) {
        throw new Error(`Environment Variable '${key}' is missing. ${issue.message}, details: ${details}`)
      } else {
        throw new Error(`Error on Environment Variable ${key} with value = ${value}: ${issue.message}`)
      }
    }
  }
}

