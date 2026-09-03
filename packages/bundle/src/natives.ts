/**
 * The two native addons, named here so that the binary carries them before anything calls
 * them.
 *
 * `bun build --compile` embeds a `.node` file the way it embeds any other module: because
 * something in the graph reaches it. Nothing does yet. The backend's routes need error codes
 * and field schemas from `@gradido/shared`, and neither of those folders touches the addon —
 * so a binary built without this file has no money arithmetic in it and no mail templates,
 * and would fail on the first transaction rather than at build time.
 *
 *   `@gradido/shared`   its root barrel re-exports `const` and `crypto`, which is what
 *                       reaches `shared-native`: decay, GradidoUnit division, hashing,
 *                       signing. The package and not the addon, because that is what
 *                       Gradido code calls — see AGENTS.md, *Amounts*.
 *
 *   `@gradido/email-native`  the templates and the SMTP client. It has no TypeScript
 *                       wrapper to go through yet, so it is named directly.
 *
 * **Delete each line when the reference stops being the only one.** An Interaction that
 * computes a decayed balance imports `@gradido/shared` itself, and one that sends a
 * registration mail imports the addon itself; a second import kept only to hold a module in
 * the graph is then a lie about why it is there.
 *
 * `import()` and not a top-level import, so an addon is *in* the binary without being
 * *loaded* by it: neither `dlopen` nor the mail templates' byte pool costs a start-up that
 * uses neither anything at all. The bundler resolves the specifier all the same — that is
 * what makes them embedded rather than looked for on the disk of whoever runs the binary.
 */
export const loadShared = async () => await import('@gradido/shared')

export const loadMail = async () => await import('@gradido/email-native')
