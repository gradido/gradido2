import * as v from 'valibot'

export type FieldSchema<T> = v.GenericSchema<T, T>

/**
 * The two rule sets a field is checked against.
 *
 * Splitting them is what lets a field say *when* each of its rules may complain, instead
 * of a central list guessing it from the kind of rule. "Too long" belongs in
 * `prevalidate` and "not a valid address yet" in `validate`, and only the field knows
 * which of its rules is which.
 */
export interface FieldRules<T> {
  /**
   * Checked on every keystroke. Only what is already wrong belongs here — a value that
   * is merely unfinished is not an error yet, and saying so reads as an accusation.
   *
   * Judge the raw value, and do not normalize in this set: trimming here would delay a
   * trailing space until the next keystroke made it an interior one.
   */
  prevalidate?: FieldSchema<T>
  /**
   * Checked for validity, and shown once the user leaves the field or reaches for the
   * submit button. This is also where normalization belongs (`v.trim()` and friends).
   *
   * It may accept values prevalidation rejects — a valid result always wins, which is
   * how an address pasted with surrounding whitespace goes straight to green instead of
   * being reported for the whitespace the trim is about to remove.
   */
  validate: FieldSchema<T>
}

/**
 * One validated form input.
 *
 * Validation state is three-valued. A form that opens with every field outlined in red
 * reads as broken, so an untouched field stays neutral. It turns red when prevalidation
 * fails, when the user leaves it, or when they reach for the submit button — and green
 * as soon as it is valid, which is the confirmation people look for while typing.
 */
export class FormField<T = string> {
  private touched = false
  private revealed = false
  private readonly rules: FieldRules<T>
  private readonly initial: T

  constructor(
    rules: FieldSchema<T> | FieldRules<T>,
    /** Text fields start out empty; every other kind has to say what empty means for it. */
    public value: T = '' as unknown as T,
  ) {
    this.rules = 'validate' in rules ? rules : { validate: rules }
    this.initial = value
  }

  set(next: T): void {
    this.value = next
  }

  /** Called on blur — from here on the field may show an error. */
  touch(): void {
    this.touched = true
  }

  /** Show whatever is outstanding, without waiting for a blur. */
  reveal(): void {
    this.revealed = true
  }

  reset(value: T = this.initial): void {
    this.value = value
    this.touched = false
    this.revealed = false
  }

  /**
   * The first issue a schema reports, as its untranslated source message. Issues come
   * back in pipeline order, so `''` reports "required" rather than "not an address".
   */
  private messageFrom(schema: FieldSchema<T> | undefined): string | undefined {
    if (!schema) {
      return undefined
    }
    const result = v.safeParse(schema, this.value)
    return result.success ? undefined : result.issues[0].message
  }

  /** Set while the value is wrong right now, rather than merely unfinished. */
  get prevalidationIssue(): string | undefined {
    return this.messageFrom(this.rules.prevalidate)
  }

  /**
   * The message to show. Prevalidation comes first: when it is what turned the field
   * red, it is also what explains it — "cannot contain spaces" is the useful sentence,
   * not the "not a valid address" that the space happens to also cause.
   */
  get issue(): string | undefined {
    return this.prevalidationIssue ?? this.messageFrom(this.rules.validate)
  }

  get valid(): boolean {
    return this.messageFrom(this.rules.validate) === undefined
  }

  /** `true` valid, `false` invalid and shown, `undefined` not judged yet. */
  get state(): boolean | undefined {
    // Valid first, on purpose: prevalidation only speaks about values the full rule set
    // has already rejected, so what normalization is about to fix is never reported.
    if (this.valid) {
      return true
    }
    if (this.prevalidationIssue !== undefined) {
      return false
    }
    if (this.touched || this.revealed) {
      return false
    }
    return undefined
  }

  /** The parsed value — trimmed and normalized by the schema, unlike `value`. */
  get parsed(): T {
    return v.parse(this.rules.validate, this.value)
  }
}

/** What `Form` needs of a field, whatever type of value it holds. */
interface AnyFormField {
  readonly valid: boolean
  reveal(): void
  reset(): void
}

/** A set of fields submitted together. */
export class Form<TFields extends Record<string, AnyFormField>> {
  constructor(public readonly fields: TFields) {}

  get valid(): boolean {
    return Object.values<AnyFormField>(this.fields).every((field) => field.valid)
  }

  /**
   * Show every outstanding problem at once — on a submit attempt, and when the pointer
   * reaches the submit button, so the answer to "why can I not submit" is already on
   * screen by the time the question is asked.
   */
  reveal(): void {
    for (const field of Object.values<AnyFormField>(this.fields)) {
      field.reveal()
    }
  }

  reset(): void {
    for (const field of Object.values<AnyFormField>(this.fields)) {
      field.reset()
    }
  }

  values(): { [K in keyof TFields]: TFields[K] extends FormField<infer V> ? V : never } {
    const result = {} as { [K in keyof TFields]: TFields[K] extends FormField<infer V> ? V : never }
    for (const name of Object.keys(this.fields) as (keyof TFields)[]) {
      const field = this.fields[name] as unknown as FormField<unknown>
      result[name] = field.parsed as never
    }
    return result
  }
}
