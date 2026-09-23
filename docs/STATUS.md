# Status

## Current state (2026-09-23, version 0.1.5)

- Builds natively (PHP 8.4 in Docker for the first build, decision 17;
  PHP 8.5.4 in WSL for 0.1.5, decision 22) and under Emscripten through
  `php-wasm-compiler`, where it is statically linked into
  `@kirigami/php-wasm`.
- `jsonk_encode()`/`jsonk_decode()`/`jsonk_validate()`/
  `jsonk_last_error()`/`jsonk_last_error_msg()`/`jsonk_get_last_errors()`/
  `JsonkException` work as designed; the `json_encode()`/`json_decode()`
  replacement mode is built (decision 19).
- Schema support: near-full draft 2020-12 (decision 15), external and
  `$id`-relative `$ref` over `ext/curl` (decisions 16, 18). Validated
  end-to-end against a real third-party schema (`kirigami.schema.json`).
- 0.1.5 fixed `enum`/`const`/`uniqueItems` never matching objects
  (decision 22).
- Performance (decision 17): decode 1.76x-2.39x faster than native
  `json_decode()`, encode without a schema roughly at parity, schema
  validation adds a second pass.
- No `.phpt` test suite yet: verification has been ad-hoc scripts per
  change (see [TODO.md](TODO.md)).

## Source files

- `php_jsonk.h` -- module header, globals (`error_code`, `error_message`,
  `last_errors`, `suppress_violations`), `JSONK_ERROR_*` enum.
- `jsonk_error.h`/`.c` -- shared error/violation reporting into
  `JSONK_G(last_errors)`, used by every other `.c`/`.cpp` file.
- `jsonk_schema.h`/`.c` -- schema compilation (yyjson -> `jsonk_schema_node`
  DAG, with "$ref" resolution and an arena-based ownership model -- see
  decision 15) and `jsonk_schema_validate_zval()` (near-full draft 2020-12
  keyword coverage as of decision 15, with "unevaluatedProperties"/
  "unevaluatedItems" tracking).
- `jsonk_regex.h`/`.c` -- reuses PHP's own bundled PCRE2 (`ext/pcre`) for
  "pattern"/"patternProperties"/format:"regex" (decision 15) -- no new
  dependency.
- `jsonk_format.h`/`.c` -- pragmatic "format" keyword checkers (decision
  15): date-time/date/time/duration, email, hostname, ipv4/ipv6, uri,
  uuid, json-pointer, regex.
- `jsonk_fetch.h`/`.c` -- external `$ref` resolution via `ext/curl`
  (decision 16): userland `curl_*()` calls through
  `call_user_function()`, a process-lifetime TTL cache, no new build-time
  dependency.
- `jsonk_decode.h`/`.cpp` -- the one C++ translation unit (decision 9),
  simdjson DOM -> zval conversion (decision 8: schema-unaware by design).
- `jsonk_encode.h`/`.c` -- zval -> yyjson mutable doc -> JSON string,
  validates against schema first when given, object-recursion detection
  (by `zend_object` handle; array/reference cycles are NOT detected in
  v1 -- see "Not done yet").
- `jsonk.c` -- `PHP_FUNCTION`s, `JsonkException`, constants, module
  plumbing, `zend_module_dep` on `"json"`/`"pcre"` (required) and
  `"curl"` (optional, decision 16).
- `config.m4`, `config.w32` -- decisions 11-12.
- `matrix.json`, `scripts/update-versions.mjs`, `package.json` -- decision
  10.
- `vendor/build/stage.sh`, `vendor/build/buildext.sh` -- decision 10/
  native build convenience (mirrors `php-mdhtml`'s own scripts).
- `.gitignore`, `LICENSE` (GPL-2.0-or-later, copied from `php-mdhtml`).
