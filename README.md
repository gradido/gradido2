# Gradido2

A rebuild of gradido legacy on a new stack, with the complete legacy feature set as the
target scope.

TypeScript is the reference implementation (`packages/`), with an additional fast
implementation of the backends in C (`fast-servers/`). Determinism-critical code — money
arithmetic, decay, signing — exists once, in C, and is used by both.

See [Architecture.md](Architecture.md) for the design and [AGENTS.md](AGENTS.md) for the
working rules.
