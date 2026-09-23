# TODO

Concrete next actions. Larger, unscheduled ideas live in
[ROADMAP.md](ROADMAP.md).

- **A `.phpt` test suite** (`run-tests.php` style, as a PHP extension is
  normally tested). Every change so far was verified with throwaway
  scripts; the cases from decisions 17-22 (flags, round-trips, `$ref`
  resolution, value equality) are the obvious first tests.
- **Cache compiled schemas.** Every `jsonk_decode()`/`jsonk_encode()`/
  `jsonk_validate()` call with a `$schema` re-parses and re-compiles it.
  A `jsonk_compile_schema(): JsonkSchema` handle, accepted in place of the
  raw JSON string, is the natural fix for hot paths validating the same
  schema repeatedly.
- **Remaining schema gaps**: `$dynamicRef`/`$dynamicAnchor`/
  `$recursiveRef` (meta-schema authoring), `$ref` with sibling keywords
  (2019-09+ semantics; `$ref` is exclusive of siblings here, draft-07
  style), a download-size cap on external `$ref` fetches, and
  conformance tests for the best-effort `unevaluatedProperties`/
  `unevaluatedItems`. `format` and `pattern` are pragmatic, not
  exhaustive grammars (decision 15).
- **Encode gaps**: `JsonSerializable` isn't honored (only public
  properties are encoded); backed enums get no special handling;
  reference cycles (`$a['x'] = &$a`) are only stopped by `$depth`, not
  reported as `JSONK_ERROR_RECURSION` (object cycles are).
- **Unimplemented `JSON_*` flags**: `JSON_INVALID_UTF8_IGNORE`/
  `_SUBSTITUTE`, the four `JSON_HEX_*`, `JSON_UNESCAPED_LINE_TERMINATORS`
  (decision 19 has the coverage table).
