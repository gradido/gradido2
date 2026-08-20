# frontend

The member-facing Gradido wallet. Mithril + Bootstrap 5, built with Vite.

The Gradido look lives in `frontend-core`, not here: design tokens in
`frontend-core/src/styles/_tokens.scss`, the configured Bootstrap build next to it, and
the brand layer on top. Values are defined once as a Sass map and emitted both as Sass
values (for Bootstrap's variable API) and as `--gdd-*` custom properties (for everything
else) — which is what keeps the CSS framework replaceable.

The typeface is Work Sans, self-hosted as one variable woff2 in
`frontend-core/src/styles/fonts/`. No Google Fonts request: the font is part of the
brand, so it ships with it rather than being fetched from a third party on every visit.

## Running it

```sh
cp .env.dist .env      # then edit it
bun install            # from the repository root
bun run dev
```

`bun run dev` compiles the message catalogs first (`bun run locale`), then starts Vite on
`DEV_SERVER_PORT`.

## Translations

Source strings are English and are marked with `t.__('…')`. German lives in
`src/locales/de/messages.po`.

- `bun run extract` rescans the code and refreshes `src/locales/messages.pot`
- `bun run locale` compiles every `.po` into `public/locales/<lang>/messages.json`

The generated JSON is build output — it is gitignored and rebuilt before `dev` and
`build`. Catalogs are fetched at runtime, so a translation fix does not need a rebuild
of the app.

## Layout

```text
src/
  client        calls to the backend
  components    reusable pieces; auth/ holds the sign-in page chrome
  config        env parsing (valibot), fails fast on a bad value
  layouts       AuthLayout — the frame around every page reachable without an account
  locales       .po source catalogs
  pages         one file per route
  styles        layout of the auth pages; the brand lives in frontend-core
```
