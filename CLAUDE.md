# php-jsonk

An ultra-fast JSON PHP extension with JSON Schema validation built in:
simdjson for decode, yyjson for encode, a hand-rolled draft 2020-12
validator. Part of the Kirigami ecosystem (github.com/php-kirigami), shipped
statically in `@kirigami/php-wasm`.

This file is the entry point only. Detailed, evolving content lives under
[docs/](docs/), split by topic:

- [docs/CONTEXT.md](docs/CONTEXT.md) — goal, sibling repos, where the
  extension is used.
- [docs/DECISIONS.md](docs/DECISIONS.md) — the numbered decision log
  (1-22). Read it before proposing a different approach to something
  already settled there; source comments cite decisions by name.
- [docs/STATUS.md](docs/STATUS.md) — what is built and verified, and the
  source file map.
- [docs/TODO.md](docs/TODO.md) — concrete next actions.
- [docs/ROADMAP.md](docs/ROADMAP.md) — ideas raised but not scheduled.
- [docs/INSTRUCTIONS.md](docs/INSTRUCTIONS.md) — building natively
  (including without Docker or installed packages) and releasing.

## Core conventions

- **Language**: conversation with the user is in French; everything
  committed to this repo (code, comments, docs, commit messages) is in
  English.
- **Target**: PHP 8.5, native and Emscripten (the `php-wasm-compiler`
  build); keep both working.
- **API**: `jsonk_encode()`/`jsonk_decode()`/`jsonk_validate()` stay the
  schema-aware entry points, separate from the opt-in `json_encode()`/
  `json_decode()` replacement mode (decisions 13, 19).
- **Dependencies**: simdjson and yyjson are vendored, fetched by
  `vendor/build/stage.sh` at `matrix.json`'s pinned versions (decision
  10); PCRE2 and curl come from PHP's own extensions.
- **Releasing**: bump `PHP_JSONK_VERSION` in `php_jsonk.h`, tag `vX.Y.Z`,
  push, then add the tag to `php-wasm-compiler`'s `matrix.json`
  ([docs/INSTRUCTIONS.md](docs/INSTRUCTIONS.md#releasing)).
- Keep this file at or under ~200 lines. New durable content goes into
  the matching `docs/*.md` file; add a decision to DECISIONS.md for any
  non-obvious "why".
