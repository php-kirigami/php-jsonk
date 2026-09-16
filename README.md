<div align="center">

<img src="https://zmotrin.github.io/assets/kirigami/kirigami-logo-universal.svg" alt="Kirigami" width="400" />

---

# php-jsonk

**An ultra-fast JSON PHP extension with JSON Schema validation built in, powered by simdjson + yyjson.**

[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-yellow)](./LICENSE)
[![PHP 8.5](https://img.shields.io/badge/php-8.5-777bb4)](https://www.php.net/releases/8.5/)
[![simdjson 4.6.11](https://img.shields.io/badge/simdjson-4.6.11-blue)](https://github.com/simdjson/simdjson)
[![yyjson 0.13.0](https://img.shields.io/badge/yyjson-0.13.0-blue)](https://github.com/ibireme/yyjson)
[![Website](https://img.shields.io/badge/website-php--kirigami.github.io-1f6b4a)](https://php-kirigami.github.io)

</div>

---

## Overview

`php-jsonk` decodes JSON via [simdjson](https://github.com/simdjson/simdjson)
(SIMD-accelerated parsing) and encodes via
[yyjson](https://github.com/ibireme/yyjson) (simdjson has no writer), and
validates against a real [JSON Schema](https://json-schema.org/) on
**both** directions:

```php
$schema = '{
    "type": "object",
    "required": ["name"],
    "properties": {
        "name": {"type": "string", "minLength": 1},
        "age":  {"type": "integer", "minimum": 0}
    }
}';

$data = jsonk_decode('{"name": "Ada", "age": 30}', $schema);
// $data is a validated stdClass; jsonk_decode() returns null and
// jsonk_get_last_errors() lists what failed if it doesn't conform.

$json = jsonk_encode($data, $schema);
// validates $data against $schema before serializing it.
```

Part of the **Kirigami** project ecosystem.

---

## Table of contents

- [php-jsonk](#php-jsonk)
  - [Overview](#overview)
  - [Table of contents](#table-of-contents)
  - [Status](#status)
  - [API](#api)
    - [`jsonk_decode()`](#jsonk_decode)
    - [`jsonk_encode()`](#jsonk_encode)
    - [`jsonk_validate()`](#jsonk_validate)
    - [Error handling](#error-handling)
  - [JSON Schema support](#json-schema-support)
  - [Building from source](#building-from-source)
  - [Requirements](#requirements)
  - [Related](#related)
  - [License](#license)
  - [Author](#author)

---

## Status

**Nothing has been compiled or run yet.** Every line was written this
session against verified real PHP/simdjson/yyjson API signatures, but no
PHP development environment was available to actually build and test it.
See [CLAUDE.md](CLAUDE.md)'s "Status" section for exactly what's done,
what's unverified, and what to do first.

---

## API

### `jsonk_decode()`

```php
jsonk_decode(
    string $json,
    ?string $schema = null,
    ?bool $associative = null,
    int $depth = 512,
    int $flags = 0
): mixed
```

Parses `$json` via simdjson. When `$schema` is given, the decoded value is
validated against it; on a violation, `jsonk_decode()` returns `null` (or
throws `JsonkException` with `JSON_THROW_ON_ERROR`) and
[`jsonk_get_last_errors()`](#error-handling) lists every violation found.

Reuses PHP's own `JSON_*` constants for `$flags` --
`JSON_OBJECT_AS_ARRAY`, `JSON_BIGINT_AS_STRING`, `JSON_THROW_ON_ERROR` are
recognized (see [CLAUDE.md](CLAUDE.md) decision 5 for the full list of
what's wired up vs. not yet).

### `jsonk_encode()`

```php
jsonk_encode(
    mixed $value,
    ?string $schema = null,
    int $flags = 0,
    int $depth = 512
): string|false
```

Validates `$value` against `$schema` (when given) *before* serializing,
then writes JSON via yyjson. Recognizes `JSON_PRETTY_PRINT`,
`JSON_UNESCAPED_SLASHES`, `JSON_UNESCAPED_UNICODE`, `JSON_FORCE_OBJECT`,
`JSON_NUMERIC_CHECK`, `JSON_PARTIAL_OUTPUT_ON_ERROR`, `JSON_THROW_ON_ERROR`.

### `jsonk_validate()`

```php
jsonk_validate(string $json, string $schema, int $depth = 512): bool
```

Decodes and validates without keeping the result -- a convenience
shorthand, not a separate fast path (see [CLAUDE.md](CLAUDE.md) decision 8
for why decode always fully builds the value today).

### Error handling

```php
jsonk_last_error(): int              // one of the JSONK_ERROR_* constants
jsonk_last_error_msg(): string
jsonk_get_last_errors(): array       // [{path, keyword, message}, ...]
```

`path` is a [JSON Pointer](https://www.rfc-editor.org/rfc/rfc6901) (e.g.
`/items/0/name`). With `JSON_THROW_ON_ERROR`, both functions throw
`JsonkException` (extends `\JsonException`) instead, which additionally
exposes `getErrors(): array` (the same list).

---

## JSON Schema support

Aims for full [draft 2020-12](https://json-schema.org/draft/2020-12)
keyword coverage for a **self-contained** schema document. See
[CLAUDE.md](CLAUDE.md) decisions 6 and 15 for the full detail and every
caveat, but in short:

**Supported**: `type` (incl. union arrays), `enum`, `const`, `required`,
`properties`, `patternProperties`, `additionalProperties` (bool or schema
form), `propertyNames`, `dependentRequired`, `dependentSchemas`,
`unevaluatedProperties` (best-effort, not yet conformance-tested), `items`,
`prefixItems` (tuples, incl. the `additionalItems` draft-07 fallback name),
`contains`/`minContains`/`maxContains`, `unevaluatedItems` (same caveat as
above), `minItems`/`maxItems`/`uniqueItems`, `minLength`/`maxLength`,
`pattern` (via PHP's own bundled PCRE2 -- ECMA-262 vs. PCRE dialect
differences are rare in practice, see CLAUDE.md), `format` (pragmatic
checkers for `date-time`/`date`/`time`/`duration`, `email`, `hostname`,
`ipv4`/`ipv6`, `uri`, `uuid`, `json-pointer`, `regex`, ...; an unrecognized
format name is annotation-only, per spec default), `minimum`/`maximum`/
`exclusiveMinimum`/`exclusiveMaximum`/`multipleOf`,
`minProperties`/`maxProperties`, `if`/`then`/`else`,
`allOf`/`anyOf`/`oneOf`/`not`, `$ref`/`$defs`/`definitions` (same-document
fragment references including self-referential/recursive schemas,
absolute `http(s)://` URLs, AND **relative references resolved against a
document's own `"$id"`**, e.g. `"../plugin-foo/options.schema.json"` --
fetched via `ext/curl`, active by default, cached for 5 minutes (APCu
when available, an in-process fallback otherwise); see
[CLAUDE.md](CLAUDE.md) decisions 16/18 for the SSRF/caching trade-offs
and the relative-`$ref` implementation -- validated end-to-end against a
real, live, third-party schema), and the boolean-schema shorthand
(`true`/`false` as a whole schema).

**Not supported, by design**: `$dynamicRef`/`$dynamicAnchor`/
`$recursiveRef` (meta-schema-authoring features); `$ref` combined with
sibling keywords (this implementation treats `$ref` as exclusive of
siblings, draft-07 style); a relative `$ref` with no known base URI (no
enclosing `"$id"`, and -- for the top-level schema specifically -- none
possible since it's just a raw string with no URL of its own) errors
clearly rather than guessing one. `contentEncoding`/
`contentMediaType` are correctly annotation-only (the spec's own
default), not a gap.

---

## Building from source

Native build (fast iteration, no Docker/Emscripten -- matches
[php-mdhtml](https://github.com/php-kirigami/php-mdhtml)'s own approach):

```bash
bash vendor/build/stage.sh      # downloads simdjson + yyjson (matrix.json-pinned)
phpize
./configure --enable-jsonk
make
```

`vendor/` is gitignored and rebuilt from source every time. There is no
Emscripten/WASM build yet.

**Statically linking into a full `php-src` tree** (rather than building
standalone via `phpize`, e.g. `php-wasm-compiler`'s static build): drop
this extension's source into `ext/jsonk/` as usual, but also stage a copy
of the vendored `vendor/simdjson/` and `vendor/yyjson/` directories at the
**root** of that `php-src` checkout. `config.m4`'s own existence check and
source paths are relative, which resolves correctly against `ext/jsonk/`
for the standalone build above (its own cwd when `configure` runs) but
resolves against the whole tree's root when `configure` is the one
generated for all of `php-src` at once. See
[php-wasm-compiler](https://github.com/php-kirigami/php-wasm-compiler)'s
`compile/php/Dockerfile` for a working example, and this repo's
[CLAUDE.md](CLAUDE.md) (decision 20) for the full diagnosis.

---

## Requirements

- PHP `>= 8.5`, built with `ext/pcre` (bundled and required since PHP 7.3
  -- `pattern`/`format` reuse it, no separate regex library needed)
- `ext/curl`, optional -- only needed for external (`http(s)://`) `$ref`
  resolution; everything else works without it
- A C **and** C++17 compiler (simdjson requires C++17), `phpize`

---

## Related

- [`simdjson`](https://github.com/simdjson/simdjson) -- the decode engine
- [`yyjson`](https://github.com/ibireme/yyjson) -- the encode engine (and
  the schema-document reader)
- [`php-mdhtml`](https://github.com/php-kirigami/php-mdhtml) -- the sibling
  extension this repo's native-first build approach is modeled on
- [`php-wasm-compiler`](https://github.com/php-kirigami/php-wasm-compiler) --
  builds the `php.wasm` runtime this extension will eventually target

---

## License

GPL-2.0-or-later. See [LICENSE](./LICENSE) for the full text.

## Author

Maxime Larrivée-Roy
