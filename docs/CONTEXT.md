# Context

Part of the Kirigami ecosystem (github.com/php-kirigami). Sibling repo to
`php-wasm-compiler` (builds the `php.wasm` runtime and vendors PHP
extensions for it) and `php-mdhtml` (the closest architectural precedent:
a from-scratch PHP extension wrapping a fast C/C++ library, built native
first, WASM later).

Goal (stated by the user, 2026-09-15): an ultra-fast JSON PHP extension
with JSON Schema validation baked into both directions:

```php
jsonk_encode($data, $schema);   // validates $data against $schema, then serializes
jsonk_decode($json, $schema);   // parses $json, then validates the result against $schema
```

The extension ships statically in `@kirigami/php-wasm` (built by
`php-wasm-compiler`, `jsonk: { mode: static }` in its `config.yaml`, with
`jsonk.replace_json_functions` flipped on for that build). Kirigami's
php-prepros `SCHEMA` class validates through `jsonk_validate()`.
