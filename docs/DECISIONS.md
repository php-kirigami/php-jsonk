# Decisions

Numbered decision log, to be respected unless explicitly revisited. The
early entries (1-16) were written before the first build; decision 17 is
where the code was first compiled and run. See [STATUS.md](STATUS.md) for
what is verified today.

1. **simdjson for decode, yyjson for encode** (user's explicit choice,
   asked directly since simdjson has no JSON-writing API at all -- see
   AskUserQuestion in this session's transcript). yyjson is also used to
   parse the `$schema` argument itself (schema documents are small; there's
   no need for simdjson's throughput there, and yyjson's read API is what
   `jsonk_schema.c` is built on). Both are vendored as small, direct source
   files (not prebuilt libraries) -- see decision 8.
2. **JSON Schema standard, not a custom DSL** (user's explicit choice).
   Scoped to a real, useful subset rather than the full draft 2020-12
   vocabulary -- seer decision 6 for exactly what's implemented vs. not.
3. **Dual build target: native PECL first, WASM second** -- exactly
   `php-mdhtml`'s own decision 6. `config.m4`/`config.w32` build a normal
   `.so`/`.dll` via `phpize`; no WASM/Emscripten work has been started.
   Whether the eventual `php-wasm-compiler` integration is `mode: static`
   (baked into core `php.wasm`, like `yaml`/`cmark`/`mdhtml`) or
   `mode: shared` (a separately loadable `@kirigami/phpext-jsonk` package,
   like `sodium`) is **not decided yet** -- revisit once the native build
   is proven, following whichever of `php-wasm-compiler`'s two mechanisms
   fits once the extension actually works.
4. **Global function names (`jsonk_encode`/`jsonk_decode`/`jsonk_validate`),
   not a namespaced API.** Unlike `php-mdhtml`'s `\MDHtml\Render()`, this
   mirrors PHP's own `json_encode()`/`json_decode()` naming directly (the
   user's own example syntax), since jsonk is positioned as a drop-in-
   shaped alternative to those, not a new namespace of its own.
5. **Reuses PHP's own `JSON_*` flag constants instead of redefining a
   parallel set** (user's explicit request: "on devrait supporter les
   flags déjà présents dans PHP genre JSON_*"). `jsonk.c` includes
   `ext/json/php_json.h` for the `PHP_JSON_*` macros (`PHP_JSON_PRETTY_PRINT`,
   `PHP_JSON_UNESCAPED_SLASHES`, `PHP_JSON_UNESCAPED_UNICODE`,
   `PHP_JSON_FORCE_OBJECT`, `PHP_JSON_NUMERIC_CHECK`,
   `PHP_JSON_PARTIAL_OUTPUT_ON_ERROR`, `PHP_JSON_OBJECT_AS_ARRAY`,
   `PHP_JSON_BIGINT_AS_STRING`, `PHP_JSON_THROW_ON_ERROR`) and declares a
   **hard module dependency on `"json"`** (`zend_module_dep`,
   `ZEND_MOD_REQUIRED`) so `JsonException` is guaranteed registered before
   `jsonk`'s own `MINIT` runs (needed for decision 6). ext/json is a
   bundled, always-on extension since PHP 8.0 (cannot be `--disable`d), so
   this dependency is safe.
   - **Deliberately not implemented in v1** (documented, not silently
     dropped): `JSON_HEX_TAG`/`_AMP`/`_APOS`/`_QUOT` (legacy inline-`<script>`
     embedding escapes -- yyjson has no equivalent, would need a custom
     string escaper), `JSON_UNESCAPED_LINE_TERMINATORS` (U+2028/U+2029
     nuance), `JSON_INVALID_UTF8_IGNORE`/`_SUBSTITUTE` (simdjson
     validates UTF-8 strictly upfront as part of its SIMD stage1 and has no
     lenient mode; yyjson's reader does support
     `YYJSON_READ_ALLOW_INVALID_UNICODE` but wiring a second decode path
     just for these two flags was judged not worth the complexity this
     round).
   - **`JSON_PRESERVE_ZERO_FRACTION` turned out not to be a "missing
     flag" but a real default-behavior BUG, caught by an actual
     round-trip test (2026-09-16, after the first successful native
     build -- see "Status")**: `jsonk_encode([...0.0...])` produced
     `"0.0"`; native `json_encode()` produces `"0"` for the same
     whole-number float by default (`JSON_PRESERVE_ZERO_FRACTION` is what
     opts OUT of that collapsing, not into it). yyjson's writer has no
     equivalent flag, but the fix needs none: `build_val()`'s `IS_DOUBLE`
     case now picks an **integer-typed** yyjson node
     (`yyjson_mut_int()`) instead of a real/double one whenever the value
     is actually whole (`d == (double)(int64_t)d`, within int64_t's exact
     range) AND `JSON_PRESERVE_ZERO_FRACTION` isn't set -- yyjson's writer
     only ever emits a decimal point for a genuinely double-typed node,
     so this reproduces PHP's own default byte-for-byte without needing
     a yyjson-level write flag. Found because a benchmark script's
     round-trip sanity check (`json_decode(json_encode($x)) ===
     jsonk_decode(jsonk_encode($x))`) came back `false` -- tracing the
     first differing row showed `price: 0.0` decoding back as `int(0)`
     via native and `float(0)` via jsonk, traced to this exact encode-side
     divergence.
6. **JSON Schema keyword coverage (v1), compiled once per call (no
   caching/reuse yet -- see "Not done yet").** `jsonk_schema.c`'s
   `jsonk_schema_compile()` builds a `jsonk_schema_node` tree from the raw
   `$schema` JSON via yyjson's read API.
   - **Implemented**: `type` (incl. union arrays like `["string","null"]`),
     `enum`, `const`, `required`, `properties`, `additionalProperties`
     (both boolean *and* schema form), `items` (single-schema form only),
     `minItems`/`maxItems`/`uniqueItems`, `minLength`/`maxLength` (counts
     Unicode codepoints, not bytes, per spec), `minimum`/`maximum`/
     `exclusiveMinimum`/`exclusiveMaximum`/`multipleOf`,
     `minProperties`/`maxProperties`, `allOf`/`anyOf`/`oneOf`/`not`, and
     the boolean-schema shorthand (`true`/`false` as a whole (sub-)schema).
   - **Not implemented**: `$ref`/`$defs`/`$anchor`/`$dynamicRef` (schema
     references), `pattern`/`patternProperties` (would need a regex engine
     -- PCRE isn't linked into this extension), `propertyNames`,
     `dependentRequired`/`dependentSchemas`, `if`/`then`/`else`,
     `contains`/`minContains`/`maxContains`, `prefixItems` (tuple
     validation), `unevaluatedProperties`/`unevaluatedItems`, `format`
     (email/date-time/uri/...), `contentEncoding`/`contentMediaType`, and
     no `$schema`/`$vocabulary`-based dialect introspection (validation is
     always "best effort" against whatever keywords are present).
   - **Equality semantics for `enum`/`const`** (`jsonk_values_equal()`):
     JSON-spec equality, not PHP's `===`/`==` -- "1 and 1.0 are considered
     equal" per the spec, regardless of PHP's own int-vs-float type
     distinction; arrays/objects compare by same length + pairwise-equal
     values (order-independent for object/assoc-array keys, index-order
     for lists).
7. **`jsonk_get_last_errors()`: a flat list of `{path, keyword, message}`,
   `path` in JSON-Pointer (RFC 6901) syntax** (`/foo/bar/0`), built
   incrementally via `jsonk_path_push()`/`jsonk_path_push_index()` (a
   `smart_str` with a save-length/restore-length "stack" pattern -- no
   extra allocation per recursion level). `anyOf`/`oneOf`/`not` probe their
   branches through a nestable suppression counter
   (`jsonk_suppress_violations_push/pop()`) so a failed branch attempt
   doesn't pollute the list with "tried this and it didn't match" noise --
   only a genuine top-level combinator failure is reported. `allOf`
   deliberately does NOT suppress: a failing `allOf` branch's real
   violation is surfaced directly, which is more useful (matches how e.g.
   ajv reports `allOf` failures).
   - `JsonkException extends JsonException` (registered against
     `ext/json`'s real `JsonException` class, looked up via
     `zend_hash_str_find_ptr(CG(class_table), "jsonexception", ...)` at
     `MINIT` -- safe because of decision 5's hard module dependency), with
     one added method, `getErrors(): array`, returning the same violation
     list. Thrown by both `jsonk_encode()`/`jsonk_decode()` when
     `JSON_THROW_ON_ERROR` is set, for BOTH a JSON syntax error and a
     schema violation (matching `json_encode`/`json_decode`'s own
     `JsonException`-for-everything convention, extended with the errors
     list).
   - `JSONK_ERROR_NONE`/`_SYNTAX`/`_DEPTH`/`_UTF8`/`_SCHEMA_INVALID`/
     `_SCHEMA_VIOLATION`/`_UNSUPPORTED_TYPE`/`_RECURSION` constants
     (`jsonk_last_error()`), independent of ext/json's own
     `JSON_ERROR_*` constants (these describe simdjson/yyjson/schema
     failures specifically, not ext/json's parser).
8. **Decode/encode + schema: two passes, not fused (v1) -- a deliberate
   simplification, not an oversight.** The obvious "ultra fast" design
   would fuse simdjson's DOM walk, schema validation, and zval-building
   into one recursive pass. Rejected for v1: it would mean writing and
   maintaining a SECOND, independent schema-checking engine specifically
   for simdjson's C++ DOM types (duplicating every keyword check already
   written for zvals in `jsonk_schema_validate_zval()`), with a real risk
   of the two engines drifting apart on edge cases over time. Instead:
   - `jsonk_decode.cpp`'s `jsonk_decode_impl()` does a **plain, schema-
     unaware** simdjson DOM -> zval conversion (fast on its own merits --
     simdjson's SIMD-accelerated parsing is the actual bottleneck removal
     PHP's native `json_decode` doesn't have).
   - `jsonk.c`'s `jsonk_decode()` then runs the **same**
     `jsonk_schema_validate_zval()` used for encode, as a second pass over
     the already-built zval tree, when a `$schema` was given. One
     validator implementation instead of two.
   - Cost: a schema-validated decode walks the value tree twice (build,
     then validate) instead of once. For the realistic payload sizes this
     targets (config files, API request/response bodies -- not
     gigabyte documents), this is a small, deliberate tax against a much
     simpler, single-source-of-truth validator. **Future optimization**:
     fuse the two passes once the two-pass version is proven correct and
     profiling shows it's worth it.
9. **The simdjson bridge is exactly one C++ translation unit
   (`jsonk_decode.cpp`), not a generic C accessor API over simdjson's DOM
   types.** Considered and rejected: exposing `extern "C"` accessor
   functions (`jsonk_dom_get_type()`, `jsonk_dom_array_at()`, ...) so the
   schema-walking logic could stay in a `.c` file. Rejected because
   `simdjson::dom::element` needs to cross the boundary by value on every
   single accessor call, and there's no safe, allocation-free way to do
   that in a generic C struct without either (a) heap-allocating a copy
   per crossing (a `new`/`delete` per array item / object field during
   decode -- real overhead, real bookkeeping risk for the C-side caller to
   get exactly right) or (b) placement-new into a fixed-size byte buffer
   with hand-verified size/alignment (fragile without being able to
   compile-test this session). Instead: `jsonk_decode.cpp` includes both
   `php.h` (which already wraps its declarations in `extern "C"` internally
   via `BEGIN_EXTERN_C`/`END_EXTERN_C`, so it's directly `#include`-able
   from a `.cpp` file) and `simdjson.h`, and does the whole DOM-walk-to-
   zval conversion in idiomatic C++ (range-based `for` over
   `dom::array`/`dom::object`, `.get()` error-code-style accessors -- no
   reliance on simdjson's exception-throwing API). It exposes exactly one
   `extern "C"` symbol, `jsonk_decode_impl()`, declared in `jsonk_decode.h`
   and called from `jsonk.c` like any other internal function.
10. **simdjson/yyjson vendored as small source files fetched fresh, not
    prebuilt libraries** -- unlike `php-mdhtml`'s `cmark-gfm` (built via
    CMake into a `.a`). Both ship in a form meant for exactly this kind of
    direct vendoring:
    - simdjson publishes a dedicated "singleheader" release asset pair,
      `simdjson.h` + `simdjson.cpp` (an amalgamation of the whole library),
      downloaded directly from the GitHub release -- no build step.
    - yyjson's entire library already lives at `src/yyjson.c` +
      `src/yyjson.h` in its repo -- fetched directly from the tagged ref.
    - `matrix.json` (root) tracks both, following `php-wasm-compiler`'s own
      convention (its CLAUDE.md decisions 11/28/33: the last entry of a
      `versions` array is what actually gets built, an honest mirror of
      upstream, refreshed via a checker script) -- **this is the first
      sibling repo outside `php-wasm-compiler` to adopt that pattern**.
      `scripts/update-versions.mjs` mirrors `compile/update-lib-versions.mjs`
      (GitHub releases/tags, `npm run update-versions[:write]`).
    - `vendor/build/stage.sh` (committed dev tooling, mirrors
      `php-mdhtml`'s own `stage.sh`) reads `matrix.json` and downloads both
      into `vendor/simdjson/` and `vendor/yyjson/` (gitignored, rebuilt
      fresh every time -- nothing vendored is hand-edited or committed).
11. **`config.m4` uses `PHP_ARG_ENABLE`, not `PHP_ARG_WITH([DIR])` like
    `php-mdhtml`.** Because simdjson/yyjson are compiled from source
    directly into the extension (decision 10), not linked as an external
    prebuilt library, there is no meaningful "point at an external DIR"
    use case the way `php-mdhtml`'s `--with-mdhtml=DIR` has for a prebuilt
    `.a`: the source files must live under `$ext_srcdir` (here,
    `./vendor/simdjson`, `./vendor/yyjson`) for PHP's build system's
    relative-path Makefile generation to work at all. Consequence for the
    eventual `php-wasm-compiler` integration (decision 3): when that
    happens, its Docker build should **copy** the fetched
    simdjson/yyjson sources into jsonk's own `vendor/` folder before
    invoking `phpize`/`@php-wasm/compile-extension` -- exactly the same
    shape `php-wasm-compiler`'s `vendorLib` mechanism already uses for
    `sodium` (its CLAUDE.md decision 32: "vendor the dependency yourself,
    place headers/.a under the extension source"), not a `--enable-jsonk=DIR`
    flag pointing somewhere external.
    - `PHP_REQUIRE_CXX()` (runs `AC_PROG_CXX`/`AC_PROG_CXXCPP` and links
      `libstdc++` automatically -- confirmed by reading `build/php.m4`'s
      real macro body rather than assuming) + `CXXFLAGS="... -std=c++17"`
      (simdjson v4.x's minimum). `yyjson.c` and every other `.c` file in
      the extension compile as plain C; `jsonk_decode.cpp` and the vendored
      `simdjson.cpp` are the only two C++ translation units.
    - `PHP_ADD_BUILD_DIR([$ext_builddir/vendor/simdjson], [1])` +
      the `yyjson` equivalent, needed because the vendored sources live in
      a subdirectory rather than the extension root -- confirmed against
      `ext/mbstring/config.m4`'s real use of the same pattern for its own
      vendored `libmbfl/` sources.
12. **`config.w32` added for Windows/PECL build parity**, mirroring
    `config.m4`'s enable-flag shape via `ARG_ENABLE` +
    `CHECK_HEADER_ADD_INCLUDE`/`ADD_SOURCES`, and `/std:c++17 /EHsc` for
    the C++ sources -- confirmed against `ext/intl/config.w32` (php-src's
    own real C++-library-wrapping extension) as the reference pattern
    rather than guessed. **Not verified against a real Windows PHP SDK
    build** (none available this session) -- see "Status".
13. **Considered, explicitly not started: replacing PHP's native
    `json_encode()`/`json_decode()` outright** (raised by the user
    mid-session: "on pourrait aussi mettre un mode pour remplacer json qui
    vient de base"). PHP has no supported way to redeclare a built-in
    global function; the only mechanism that could make `json_encode()`
    itself resolve to jsonk's implementation is overwriting its entry in
    the internal function table at `MINIT` (a technique some real-world
    extensions use, but a genuinely invasive one -- version-dependent
    internals, risk of surprising anything that inspects/caches function
    pointers, opcache/JIT interaction, and it would silently change the
    behavior of every other extension/userland code calling
    `json_encode()`, not just code that opted in). Not attempted. If this
    is wanted, it needs its own explicit decision (probably as an opt-in
    `jsonk.override_json_functions` INI setting, not a default) before any
    code is written -- flagged here so it isn't forgotten, not designed.
    **Clarified by the user, same day**: idea not abandoned, but whatever
    form it eventually takes, `jsonk_encode()`/`jsonk_decode()` (the
    schema-validating entry points) stay as their own separate functions
    regardless -- an override mode would only ever affect the plain,
    schema-less `json_encode()`/`json_decode()` call sites, never replace
    or fold into the schema-aware API.
14. **Queued for a future round, not started: JSONPath query support**
    (raised by the user mid-session: "Ça serait nice aussi ajouter une
    fonctionnalité pour du jsonpath"). **Real head start found while
    reading simdjson's DOM API for decision 9**: `simdjson::dom::element`
    already exposes `.at_pointer()` (RFC 6901 JSON Pointer) AND
    `.at_path()` (RFC 9535 JSONPath, scoped to "key names and array
    indices") natively, plus `.at_path_with_wildcard()` for `*` wildcard
    support. A future `jsonk_query(string $json, string $path)` could
    largely be a thin wrapper over these rather than a JSONPath engine
    written from scratch -- worth checking first whether that native
    subset covers what's actually needed before reaching for a full
    RFC 9535 implementation (filters, slices, recursive descent `..` are
    NOT covered by simdjson's built-in support and would need real work).

15. **The hand-rolled validator was extended toward full draft 2020-12
    keyword coverage, rather than switching to a library** (user,
    2026-09-15: "On va garder le schema maison, mais on va essayer de le
    faire correspondre à 100% des schemas réels" -- a direct follow-up to
    a discussion of whether to integrate `valijson` instead; kept the
    homemade validator, closed most of decision 6's "not implemented"
    list). "100%" here means every keyword that can be evaluated against
    a **self-contained** schema document -- no network fetch for external
    `$ref`s, see below.
    - **`pattern`/`patternProperties`/format:`"regex"` reuse PHP's own
      bundled PCRE2** (`jsonk_regex.c`/`.h`) instead of vendoring a regex
      engine -- ext/pcre is a hard, always-compiled-in PHP dependency
      since 7.3 (like ext/json, decision 5), so this adds zero new
      dependencies. Confirmed the exact calling convention (
      `pcre_get_compiled_regex_cache()` → `php_pcre_pce_re()` →
      `php_pcre_create_match_data()` → `pcre2_match()` →
      `php_pcre_free_match_data()`) against real callers in php-src itself
      (`ext/filter/logical_filters.c`, and `ext/spl`/`ext/zip`/`ext/pgsql`
      also depend on `ext/pcre/php_pcre.h` the same way) rather than
      guessing. New hard module dependency on `"pcre"`
      (`ZEND_MOD_REQUIRED`, alongside `"json"`).
      - **Known, documented dialect gap**: JSON Schema's `pattern` is
        ECMA-262 (JavaScript) regex syntax; PCRE isn't byte-identical
        (mostly obscure Unicode property escape names and some lookbehind
        edge cases differ). Ordinary character classes/anchors/
        quantifiers/groups -- what real-world patterns actually use --
        behave the same under both.
      - **Known, accepted noise**: `pcre_get_compiled_regex_cache()` emits
        a PHP `E_WARNING` on a malformed pattern, same as a broken
        `preg_match()` call would. Not silenced (would need swapping
        `EG(error_reporting)` or the error-handling mode around the call,
        not attempted this round).
    - **`format`** (`jsonk_format.c`/`.h`): pragmatic, structural
      checkers for `date-time`/`date`/`time`/`duration`, `email`/
      `idn-email`, `hostname`/`idn-hostname`, `ipv4`, `ipv6` (handles
      `::` compression and an embedded IPv4 tail), `uri`/`iri`,
      `uri-reference`/`iri-reference`, `uuid` (via the new regex engine),
      `json-pointer`, `relative-json-pointer`, `regex`. Deliberately NOT
      exhaustive RFC-grammar parsers (e.g. `email` doesn't implement RFC
      5322 in full -- nobody's format checker does in practice). An
      **unrecognized** format name (`uri-template`, dialect extensions,
      typos) always passes -- annotation-only, matching the spec's own
      default for a format a validator doesn't implement.
    - **`$ref`/`$defs`/`definitions`, same-document only**
      (`compile_ref()` in `jsonk_schema.c`): resolves via yyjson's
      **built-in** JSON Pointer support (`yyjson_doc_ptr_getn()`, RFC
      6901) -- a real find while reading yyjson.h for this, avoided
      writing a pointer resolver by hand. Only a fragment reference
      (`"#"` or `"#/json/pointer"`) resolves; anything else (an absolute
      or relative URI) is a compile-time `JSONK_ERROR_SCHEMA_INVALID`
      with a clear message, not a silent wrong pass -- no network fetch,
      by design.
      - **Sibling keywords alongside `$ref` are ignored** (the returned
        node IS the resolved target directly, not a wrapper merging
        `$ref` with anything else in the same schema object). This is
        draft-07-era `$ref` semantics, not 2019-09+'s "`$ref` can coexist
        with siblings" -- a deliberate, documented gap: real-world
        schemas overwhelmingly use `$ref` alone, and supporting siblings
        would also have made `unevaluatedProperties`/`unevaluatedItems`
        tracking (see below) interact with `$ref`, which this design
        specifically avoids needing to handle.
      - **Recursive/self-referential schemas work** (e.g. a tree or
        linked-list shape referencing its own `$defs` entry): the
        compiler registers a `$ref` target's node in a cache BEFORE
        recursively filling it in, so an inner reference back to the same
        target resolves to that same (still-being-filled) pointer instead
        of infinite-recursing during *compilation*. *Validating* against
        such a schema is still bounded by the new `depth_remaining`
        parameter (see below), not by anything `$ref`-specific.
      - **Real architectural consequence, found while designing this**:
        `$ref` makes the compiled schema a DAG (possibly with cycles),
        not a strict tree -- multiple `$ref` sites can share the exact
        same compiled node. `jsonk_schema_free()`'s old "recursively free
        every child pointer" approach would double-free a shared node, or
        infinite-loop on a cyclic one. Fixed by switching to an **arena
        model**: every `jsonk_schema_node` allocated during a compile is
        tracked in a flat list (`jsonk_compile_ctx.arena`), and
        `jsonk_schema_free()` frees every arena entry exactly once by a
        flat loop, never by following a child pointer to decide what to
        free. Bonus: this also **closes decision 6's "leak on compile
        failure" note** -- a failed compile now walks the same arena and
        frees everything allocated so far, cleanly, on every path.
    - **`if`/`then`/`else`, `contains`/`minContains`/`maxContains`,
      `prefixItems`** (2020-12 tuple form, with `items` now applying only
      from `prefixItems`'s length onward -- draft-07's `additionalItems`
      is accepted as a fallback name when `items` itself is absent, for
      older real-world schemas), **`propertyNames`, `dependentRequired`,
      `dependentSchemas`**: implemented following the spec directly, nothing
      unusual -- see `jsonk_schema.c` for the keyword-by-keyword logic.
    - **`unevaluatedProperties`/`unevaluatedItems`** -- the hardest
      keyword in the whole spec to get right even for mature validators.
      Implemented via an "evaluated-tracking" side channel:
      `jsonk_schema_validate_zval()` gained two new optional out-params
      (`evaluated_props`/`evaluated_items`, `HashTable*`, nullable) that a
      validate call fills with every property key / array index it (and
      any of its own successfully-matching `allOf`/`anyOf`/`oneOf`/
      `dependentSchemas`/`if-then`/`if-else` branches) evaluated. A new
      `probe_branch()` helper runs a combinator branch, and -- only when
      it actually matched -- merges what IT evaluated up into the
      caller's own accumulating set (annotations from a non-matching
      branch are correctly discarded, per spec; `not`'s annotations are
      always discarded, win or lose, also per spec). `unevaluatedItems`/
      `unevaluatedProperties` themselves then run LAST (after
      properties/patternProperties/additionalProperties/contains/
      if-then-else/allOf/anyOf/oneOf have all contributed), checking only
      what's left outside the fully-merged evaluated set. Top-level
      callers (`jsonk.c`, `jsonk_encode.c`) pass `NULL,NULL` -- they only
      want a pass/fail, not the evaluated set itself.
      - **Honest caveat**: this is a best-effort implementation of the
        single most subtle part of the 2020-12 spec. It covers the
        realistic, common interaction (`properties`/`patternProperties`/
        `additionalProperties`/`items`/`prefixItems`/`contains`/
        `if-then-else`/`allOf`/`anyOf`/`oneOf` all contributing to one
        `unevaluatedProperties`/`unevaluatedItems` at the same schema
        level). It has **not** been checked against the official
        JSON Schema Test Suite (no PHP build to run it against yet -- see
        "Status"), and `unevaluatedProperties`/`Items` is exactly the
        keyword where even well-established validators occasionally
        disagree on edge cases. Treat this as "best real-world effort,"
        not "spec-perfect," until it's been run against real conformance
        tests.
    - **`contentEncoding`/`contentMediaType`/`contentSchema`: intentionally
      not implemented at all**, and this is spec-correct, not a gap --
      the spec itself defines these as annotation-only by default (a
      validator MAY assert them, most don't). `compile_node()` simply
      never looks these keys up, so they're silently accepted as
      annotations, exactly like `title`/`description`/`default`/
      `examples`/`$comment`/`$id`/`$schema`/`$anchor` and any other
      keyword this implementation doesn't recognize.
    - **`$dynamicRef`/`$dynamicAnchor`/`$recursiveRef`** (advanced
      meta-schema-authoring features, used almost exclusively by
      meta-schemas themselves, not real-world application schemas): not
      implemented. A schema using these fails to compile with a clear
      `JSONK_ERROR_SCHEMA_INVALID` (they're not recognized as `$ref` --
      only the literal `$ref` keyword is handled -- so they're silently
      ignored as unknown keywords rather than erroring, which is the
      correct "annotation" fallback for an unrecognized keyword, but
      means a schema relying on them for actual constraint enforcement
      will validate more *permissively* than a fully spec-compliant
      validator would. Documented here rather than silently discovered
      later.)
    - **Validation depth is now enforced everywhere, not just in decode/
      encode** (a real gap this round closed): `jsonk_schema_validate_zval()`
      gained a `depth_remaining` parameter, decremented on every recursive
      step -- array items, object properties, `$ref` indirection,
      `allOf`/`anyOf`/`oneOf`/`not`/`if`/`then`/`else`/`dependentSchemas`
      branches, `contains`, `unevaluatedItems`/`unevaluatedProperties`.
      This guards against both a genuinely deep value AND a pathological
      schema (e.g. two `$defs` entries that `$ref` each other with no
      other constraint, which would otherwise recurse forever validating
      even a single scalar). Threaded from the same `$depth` argument
      `jsonk_decode()`/`jsonk_encode()`/`jsonk_validate()` already expose.
    - **External `$ref` was out of scope at the time this decision was
      written -- superseded the same day by decision 16.** Every other v1
      gap from decision 6 is closed as of this decision.

16. **External `$ref` (an http(s) URL, not just a same-document fragment)
    implemented via PHP's own `ext/curl`, called through userland
    functions -- not by linking libcurl directly** (user, 2026-09-15:
    "Pour le $ref, on peut utiliser libcurl qui sera déjà présent dans le
    build de php"). `jsonk_fetch.c`/`.h`: `curl_init`/`curl_setopt`/
    `curl_exec`/`curl_getinfo`/`curl_close` are called via
    `call_user_function()` (the classic Zend "call a global function by
    name" API), with `CURLOPT_*`/`CURLINFO_*` option numbers resolved at
    runtime through PHP's own registered constants
    (`zend_get_constant_str()`) rather than hardcoded libcurl enum values
    -- deliberately, after checking curl's real `curl.h` and finding the
    numeric values aren't simply grep-able (`CINIT()` macro arithmetic),
    so guessing them was a real correctness risk worth avoiding entirely.
    This needs **zero new build-time dependency**: no libcurl headers/lib
    to link against, `config.m4`/`config.w32` are unchanged beyond adding
    `jsonk_fetch.c` to the source list. `ext/curl` is only an **optional**
    module dependency (`ZEND_MOD_OPTIONAL`, unlike the hard `json`/`pcre`
    ones from decisions 5/15) -- most of jsonk works fine without it; an
    external `$ref` just fails with a clear `JSONK_ERROR_SCHEMA_INVALID`
    if it's absent, checked via `zend_hash_str_exists(&module_registry,
    "curl", ...)`.
    - **Two explicit trade-off questions asked and answered before writing
      any code** (both affect behavior meaningfully enough to not just
      decide silently):
      - **SSRF posture: active by default, no opt-in flag.** Fetching a
        URL found inside a `$schema` string is a real SSRF vector if that
        schema can come from an untrusted source (e.g. supplied by an
        API's own caller) -- the user chose simplicity over a safety
        rail here, explicitly. **Real hardening added regardless**, since
        it doesn't trade off against ease of use: `CURLOPT_PROTOCOLS` and
        `CURLOPT_REDIR_PROTOCOLS` are hard-restricted to `CURLPROTO_HTTP|
        CURLPROTO_HTTPS` (resolved the same runtime-constant way as every
        other option) -- without this, `CURLOPT_FOLLOWLOCATION` (needed
        for ordinary HTTP redirects) could otherwise follow a redirect
        into `file://`, a local-file-read vector, which would be a much
        worse outcome than a bounded SSRF. `CURLOPT_SSL_VERIFYPEER`/
        `_VERIFYHOST` are also on (no silent downgrade), and
        `CURLOPT_TIMEOUT`/`CURLOPT_CONNECTTIMEOUT` bound how long a
        pathological/unresponsive target can block the request.
      - **Caching: a process-lifetime, TTL-based (5 minutes,
        `JSONK_FETCH_CACHE_TTL`) in-memory cache**, not "no cache" and not
        a full compiled-schema cache (which doesn't exist yet -- see
        "Not done yet"). Lives in `PHP_MINIT`/`PHP_MSHUTDOWN` (module
        lifetime), deliberately NOT `RINIT`/`RSHUTDOWN` (request
        lifetime) -- the whole point is surviving across requests in a
        persistent worker (traditional non-ZTS PHP-FPM/CLI) so the same
        external schema isn't refetched on every single
        `jsonk_decode()`/`jsonk_encode()` call that references it.
        Consequence: every allocation the cache owns (the HashTable
        itself, each cached entry's `zend_string` body) must use PHP's
        **persistent** allocator (`pemalloc`/`zend_string_init(..., 1)`),
        not the ordinary request-scoped one -- request-scoped
        (`emalloc`) memory is invalid the instant the request that
        allocated it ends, and this cache is specifically meant to
        outlive the request that populated it.
        - **Known, accepted limitation: not thread-safe.** No lock guards
          the cache's HashTable. Correct and safe for the SAPIs that
          actually matter most in practice (traditional PHP-FPM, the CLI
          SAPI -- both non-ZTS, one worker process per request, no
          shared-memory race possible), but a genuinely threaded SAPI
          (a ZTS build, e.g. an embedded multi-threaded context or a
          threaded Apache MPM) could race on concurrent access. Flagged
          rather than silently ignored; real locking is future work if
          this ever matters for a ZTS deployment.
        - **Superseded in part the same week (2026-09-16, user's
          suggestion: "Si APCu garde sa cache entre les requêtes à la VM,
          ça serait une avenue à considérer pour la cache")**: `jsonk_fetch_url()`
          now prefers **APCu** over the hand-rolled cache above whenever
          it's actually usable -- checked via `apcu_enabled()` (a real
          APCu function for exactly this, since the extension can be
          loaded but functionally disabled, e.g. the CLI SAPI's default
          `apc.enable_cli=0`), not just "is the module loaded." APCu
          fixes the thread-safety caveat outright (its shared-memory
          segment has its own locking) and needs no persistent-allocator
          bookkeeping of our own -- `apcu_store($key, $value,
          JSONK_FETCH_CACHE_TTL)` handles both storage and expiry
          natively. Called the same way as `ext/curl` (userland
          `apcu_fetch`/`apcu_store` via `call_user_function`, keys
          prefixed `jsonk:fetch:`) -- no new build-time dependency, and no
          `zend_module_dep` entry either (softer than even `curl`'s
          `ZEND_MOD_OPTIONAL`: this is a pure runtime nice-to-have with no
          MINIT-ordering requirement, so a plain availability check at
          call time is enough). The hand-rolled process-global cache from
          the paragraph above is kept as the fallback for when APCu isn't
          loaded/enabled, not deleted.
    - **`compile_ref()` (in `jsonk_schema.c`) extended, not replaced**:
      still resolves a bare `"#..."` fragment against the top-level
      schema document exactly as before; a `"$ref"` starting with
      `http://`/`https://` now splits off any trailing `"#/json/pointer"`
      fragment, fetches the URL part via `jsonk_fetch_url()`, parses the
      response as its own independent `yyjson_doc`, and resolves the
      fragment (or the fetched document's root, if there's no fragment)
      within THAT document -- freed once `fill_node_from_schema()` has
      copied everything it needs out of it, same lifetime discipline as
      the top-level schema document.
    - **The "bare fragment nested inside an external fetch" gap was fixed
      the same day**, without building the full "$id"-based base-URI
      resolution algorithm the spec technically describes (still judged
      not worth it): `jsonk_compile_ctx` gained a `current_doc` field
      (`root_doc` at the top level), which `compile_ref()` temporarily
      swaps to the externally-fetched `yyjson_doc` for the duration of
      compiling ITS content, restoring it afterward -- so a bare
      `"#/..."` fragment found inside a fetched document now resolves
      against that document, not the original top-level one. `ref_cache`
      keys are qualified by `current_doc`'s own pointer identity
      (`build_ref_cache_key()`) so the same bare fragment text appearing
      in two different documents can't collide. **Still not a full
      base-URI algorithm**: no support for a document declaring its own
      `$id` that a RELATIVE (as opposed to bare-fragment or absolute)
      external reference would resolve against -- real-world schemas
      needing that are rare enough this wasn't chased further.
    - **Also not implemented, and not planned for this round**: a
      download-size cap on the fetched response (a slow-but-small
      response is bounded by `CURLOPT_TIMEOUT`; a small-but-huge one
      currently is not, since `CURLOPT_RETURNTRANSFER` buffers the whole
      body before `jsonk_fetch_url()` ever sees it -- capping this
      properly needs a `CURLOPT_WRITEFUNCTION` callback wired through
      `zend_call_function`, real added complexity deferred for now).

17. **✅ First real native build, 2026-09-16 -- "Docker est libre" (user).**
    Every decision above was written against verified real API signatures
    but had never actually been compiled. Built and tested for real using
    a throwaway `php:8.4-cli` Docker container (PHP 8.5 has no official
    Docker Hub image yet; 8.4 already ships `phpize`, `ext/curl` and
    `ext/pcre` -- close enough in Zend API vintage to be a genuine
    smoke test, not a stand-in for the real PHP 8.5 target) with
    `build-essential`/`g++`/`nodejs`/`git` installed on top. `vendor/build/
    stage.sh` worked unmodified on the first try. **Eight real, distinct
    bugs were found and fixed** -- exactly the value of actually
    compiling instead of trusting API research alone:
    1. **Every `.c`/`.cpp` file except `jsonk.c` was missing
       `#include "php.h"`** (and the `HAVE_CONFIG_H` guard) before
       `php_jsonk.h` -- `php_jsonk.h` itself deliberately doesn't
       self-include `php.h` (matching real php-src convention, e.g.
       `ext/json/php_json.h`), so every translation unit must do it
       itself. `zend_module_entry`/`zend_string`/`zval`/`bool` etc. were
       all undefined in six files. Fixed by adding the standard preamble
       everywhere.
    2. **`zend_string_init(ZEND_STRL(...))` in `jsonk_error.c`** -- missing
       the required third `persistent` argument (`ZEND_STRL` only expands
       to 2 of the 3 params `zend_string_init` needs).
    3. **`jsonk_schema_node` was missing its own `arena`/`arena_count`
       fields** -- decision 15's arena-based freeing design was fully
       *implemented* in `jsonk_schema.c` (both fields read/written) but
       the fields were never actually *declared* on the struct in
       `jsonk_schema.h`.
    4. **A malformed comment in `jsonk_fetch.c`**: `/* Resolves a
       CURLOPT_*/CURLINFO_* name ... */` -- the literal `*/` inside
       `CURLOPT_*/CURLINFO_*` closed the comment early, exactly the same
       failure shape `php-wasm-compiler`'s own CLAUDE.md already
       documents twice for unrelated files (an em dash, a `*/`-shaped
       docstring glob) -- a real, recurring hazard of writing comments
       that happen to contain those two characters adjacent to each
       other.
    5. **The `.so` compiled and linked but failed to *load***:
       `undefined symbol: _ZTVN10__cxxabiv120__si_class_type_infoE` (a
       C++ RTTI vtable symbol). `PHP_REQUIRE_CXX()`'s own
       `PHP_ADD_LIBRARY([stdc++])` call (no target variable) did not
       reach this extension's actual final link line -- confirmed by
       reading the real `cc -shared ...` command in the build log, which
       had no `-l` flags at all. Fixed by explicitly binding it via
       `PHP_ADD_LIBRARY(stdc++, 1, JSONK_SHARED_LIBADD)` +
       `PHP_SUBST(JSONK_SHARED_LIBADD)` in `config.m4`.
    6. **A real string-corruption bug in schema violation reporting**,
       caught by actually reading a `jsonk_get_last_errors()` result
       rather than just checking counts: a violation's `path` field came
       back as `"/ageing required property \"name\""` -- garbage
       trailing a valid `/age` prefix. Root cause: `smart_str`'s
       "pop a path segment" idiom used throughout `jsonk_schema.c`
       (`ZSTR_LEN(path->s) = saved_len;`) only shrinks the *logical*
       length; it never re-terminates the buffer with `\0` at the new
       shorter length. `jsonk_add_violation()` reads that pointer with
       `add_assoc_string()`, which uses `strlen()` -- so it walked straight
       past the truncation point into a stale, longer message that had
       previously occupied the same buffer. Fixed by having
       `current_path()` call `smart_str_0()` (safe: smart_str never
       shrinks its real allocation, only `ZSTR_LEN`) before returning the
       pointer.
    7. **The single most impactful bug: `IS_INDIRECT` object properties
       were never dereferenced**, breaking `jsonk_encode()`/schema
       validation for *any* object with a declared (typed or plain
       `public $x;`) property -- not an edge case, the ordinary case for
       real PHP classes. `Z_OBJPROP_P()`'s returned property table
       commonly stores declared properties as `IS_INDIRECT` zvals
       (pointing into the object's own storage) rather than the value
       directly; the existing "dereference `IS_REFERENCE`" logic at the
       top of `build_val()`/`jsonk_schema_validate_zval()` didn't handle
       this different, non-reference indirection at all, so every such
       property silently became `JSONK_ERROR_UNSUPPORTED_TYPE`. Caught by
       a deliberately recursive test fixture (`class Node { public
       $self; } $n->self = $n;`) meant to test recursion detection, whose
       error code came back wrong (`UNSUPPORTED_TYPE` instead of
       `RECURSION`) -- tracing *why* surfaced the real, much bigger bug
       underneath. Fixed by dereferencing `IS_INDIRECT` (via
       `Z_INDIRECT_P()`, re-checking `IS_REFERENCE` afterward since an
       indirect slot can itself hold a reference) at both entry points,
       rather than patching each individual `ZEND_HASH_FOREACH_STR_KEY_VAL`
       call site with the `_IND` macro variant -- centralizing it this
       way also covers `zend_hash_find()`-based property lookups (the
       `"properties"` keyword), which have no `_IND` macro equivalent to
       lean on.
    8. **`JSON_PRESERVE_ZERO_FRACTION` was a real default-behavior bug,
       not a "missing flag"** -- see the updated note earlier in this
       file (decision 5) for the full write-up. Found by a benchmark
       script's own round-trip sanity check
       (`json_decode(json_encode($x)) === jsonk_decode(jsonk_encode($x))`)
       coming back `false`; bisecting the first differing row led
       straight to it.
    - **Full functional test coverage, not just "it loads"**: two test
      scripts (`test1.php`/`test2.php`, scratch, not committed) exercised
      basic encode/decode, associative vs. object decode, schema
      valid/invalid decode and encode, `jsonk_validate()`,
      `JSON_THROW_ON_ERROR` + `JsonkException::getErrors()`, malformed-JSON
      syntax errors, `pattern`/`format:"email"`, object recursion
      detection, `uniqueItems`/`multipleOf`/`contains`/`prefixItems`/
      `dependentRequired`/`propertyNames`/`if`-`then`-`else`/
      `unevaluatedProperties`, **and a real external `$ref`** (a PHP
      built-in server inside the container serving a schema file whose
      own `$ref: "#/$defs/zip"` fragment resolves against *that* fetched
      document, not the top-level one -- the exact case decision 16's
      `current_doc` fix targets) -- every one of these passed once the
      eight bugs above were fixed.
    - **Benchmarked against native `json_encode()`/`json_decode()`**
      (user: "Tu benchmarkeras pour tester avec le json_encode built-in"),
      3 payload sizes (100/1000/5000 rows of realistic mixed-type
      records), `hrtime()`-based. Honest, not uniformly "faster":
      - **Decode is consistently faster than native**, 1.76x-2.39x
        (schema-validated decode included) -- simdjson's real advantage
        shows up clearly and consistently across all three sizes.
      - **Encode without a schema is roughly at parity with native**,
        trending slightly *slower* at larger sizes (0.86x-1.36x, noisy at
        small N) -- architecturally expected: `jsonk_encode()` builds a
        full intermediate `yyjson_mut_doc` tree before serializing it,
        while PHP's own encoder writes bytes directly from zvals in a
        single pass with no intermediate tree. yyjson's raw writer speed
        doesn't fully offset that extra allocation/traversal pass at this
        payload shape.
      - **Schema validation adds real, substantial overhead**, as
        expected from decision 8's "two passes, not fused" architecture:
        encode+schema lands at roughly 0.33x-0.55x of native's plain
        encode speed (i.e. 2-3x slower) since it's a full extra tree walk
        before encoding even starts. Decode+schema is cheaper by
        comparison (simdjson's decode-side speed advantage absorbs most
        of the extra validation pass) and still usually beats plain
        native decode (1.17x-2.39x).
      - This is real, first-party performance data, not a marketing claim
        -- "ultra fast" holds clearly for decode, is genuinely mixed for
        encode, and schema validation is priced honestly as a real cost,
        not free. Worth revisiting once schema compilation gets its own
        cache (a currently-uncached schema is recompiled from scratch on
        every single call, which these benchmarks reuse across
        iterations via literal reuse of the same PHP `$schema` string but
        NOT via any actual jsonk-side caching -- see "Not done yet" --
        so the schema-validated numbers above already reflect that
        recompilation cost every iteration, arguably the more realistic
        worst case for a repeated hot path today).

18. **Relative `$ref` support added ("$id"-based), same session, and
    validated end-to-end against a real, live, third-party schema
    (2026-09-16, user: "On supporte les $refs relatifs ?" -> "on le fait
    tout de suite").** Until this decision, `compile_ref()` only handled
    a bare same-document fragment or an ABSOLUTE `http(s)://` URL -- a
    `"$ref"` like `"../plugin-foo/options.json"` (extremely common in any
    real multi-file schema set) hit the hard "must be a fragment or an
    http(s) URL" error. Implemented the spec's own actual mechanism for
    this rather than inventing something ad hoc:
    - **`jsonk_compile_ctx` gained `current_base_uri`** (a `zend_string*`,
      NULL = unknown), swapped alongside `current_doc` the same way
      (decision 16). The top-level schema's own base URI comes from its
      own root `"$id"` if it declares one (checked once in
      `jsonk_schema_compile()`), else NULL -- a raw schema string handed
      to `jsonk_decode()`/`jsonk_encode()` has no URL of its own, so
      without an `"$id"` a relative `"$ref"` at the top level has nothing
      to resolve against and is a clear compile error, not a guess. An
      externally-fetched document's own base URI is its own `"$id"` if it
      declares one (per spec, `"$id"` always wins over the URL it was
      actually fetched from), else that fetch URL itself.
    - **`resolve_relative_uri()`**: a new, from-scratch RFC 3986 SS5.3
      implementation (merge + remove-dot-segments) -- handles the real
      cases multi-file schemas actually use (`"sibling.json"`,
      `"../parent-dir/other.json"`, an absolute-path reference starting
      with `/`) via careful, traced-through-by-hand string manipulation
      (not a vendored URI library -- this is genuinely small/self-
      contained enough not to justify one). Deliberately not a full RFC
      3986 parser: no query-string, scheme-relative (`"//host/path"`), or
      authority-component handling beyond a plain relative path -- not
      needed for what real schema files reference.
    - **Validated against a real, live, third-party schema the user
      pointed at directly**: `https://cdn.jsdelivr.net/gh/php-kirigami/
      kirigami@main/packages/kirigami/kirigami.schema.json` (real
      declares `"$id"` equal to its own jsdelivr URL, and has three
      relative `"$ref"`s -- `"../plugin-highlight/options.schema.json"`,
      `"../plugin-extlink/..."`, `"../plugin-embed/..."`). `jsonk_validate()`
      against this exact schema, with a minimal valid `kirigami.yaml`-
      shaped payload, correctly fetched and resolved all three relative
      refs and validated `true`; an invalid payload (missing the required
      top-level `"kirigami"` key) correctly validated `false`. **Also a
      real proof of decision 16's fetch cache actually paying off in
      practice, not just in a synthetic benchmark**: first call took
      ~2.9s (three genuine network round trips to jsdelivr for the three
      relative refs); an immediately repeated second call against the
      exact same schema took **0.9ms** -- a >3000x speedup from the cache
      alone, measured, not estimated.
    - **Known limitation, same shape as decision 16's original one**: a
      relative `"$ref"` needs SOME known base URI (its own document's
      `"$id"`, or -- one level up -- whatever base URI THAT document
      itself resolved against) -- a document with neither an `"$id"` of
      its own NOR having been reached via an external fetch (i.e., the
      top-level schema string itself, passed with no `"$id"`) simply has
      no base a relative ref could resolve against, and errors clearly
      rather than guessing one.

19. **`json_encode()`/`json_decode()` replacement built (decision 13,
    finally implemented), plus phpinfo reporting for curl/pcre/apcu and
    the replacement's own status, and a real double-free bug found and
    fixed along the way (2026-09-16, user: "Ça supporte assez bien tous
    les flags JSON_* de php ? Est-ce qu'on a fait le remplacement de
    json_encode/decode en cas que json soit désablé ?").**
    - **First: confirmed `ext/json` genuinely cannot be disabled in PHP
      8.0+** (`ext/json/config.m4` has no `PHP_ARG_ENABLE` guard at all --
      it's unconditionally compiled) -- so "in case json is disabled"
      isn't a real runtime condition to detect, on native PHP or on the
      WASM build. This makes the replacement an opt-in *choice*
      (jsonk's simdjson/yyjson backends instead of ext/json's own parser/
      serializer, for whoever wants that), not a fallback for a scenario
      that can't happen -- confirmed directly with the user, who agreed
      the ini flag stays opt-in rather than becoming an automatic
      "detect and replace" mechanism.
    - **`jsonk.replace_json_functions`** (`PHP_INI_SYSTEM`, default off) --
      `PHP_MINIT_FUNCTION(jsonk)` clones jsonk's own already-registered
      `jsonk_json_encode_replacement`/`jsonk_json_decode_replacement`
      `zend_internal_function` structs (built correctly by PHP's own
      `zend_register_functions()`) directly into the `json_encode`/
      `json_decode` slots of `CG(function_table)` via
      `zend_hash_str_update_mem()`. The two replacement `PHP_FUNCTION`s
      match native `json_encode()`/`json_decode()`'s signatures exactly
      (no `$schema` param -- this never touches the schema-aware
      `jsonk_encode()`/`jsonk_decode()` API, a hard separation the user
      set earlier in this project), call `jsonk_encode_impl`/
      `jsonk_decode_impl` with `schema=NULL`, write errors into
      `JSON_G(error_code)` (ext/json's own global, so `json_last_error()`/
      `json_last_error_msg()` keep working unmodified) via a new
      `jsonk_error_to_php_json_error()` mapping, and throw
      `JsonkException` on `JSON_THROW_ON_ERROR` (matching native's own
      `JsonException`, since `JsonkException extends JsonException`).
    - **Two independent, real double-free crashes found and fixed by
      actually running it** (`double free or corruption (!prev)`, SIGABRT,
      at PHP shutdown -- functional behavior itself was correct in both
      cases, the crash only hit during the function table's teardown):
      1. **`function_name` sharing**: `zend_hash_str_update_mem()`'s raw
         byte-copy left the clone's `function_name` pointing at the exact
         same `zend_string` the original `jsonk_json_encode_replacement`/
         `_decode_replacement` entries still own; `zend_function_dtor()`
         (`Zend/zend_opcode.c`) calls `zend_string_release_ex()` on it
         once per owning struct -> double release. Fixed by giving each
         clone its own `zend_string_init(ZEND_STRL("json_encode"/
         "json_decode"), 1)`.
      2. **`arg_info` sharing (the deeper, non-obvious one)**: fixing (1)
         didn't stop the crash. Read the real
         `Zend/zend_API.c`/`Zend/zend_opcode.c` source
         (`zend_register_functions()`/`zend_free_internal_arg_info()`) to
         find the actual cause: any internal function declaring real
         parameter/return types (`ZEND_ACC_HAS_TYPE_HINTS`/
         `ZEND_ACC_HAS_RETURN_TYPE` -- true for both replacement
         functions, they use `ZEND_ARG_TYPE_INFO`/
         `ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX`) gets its `arg_info`
         pointer **replaced with a fresh `malloc()`'d heap copy** at
         registration time -- it does NOT stay pointing at the
         macro-declared static array, contrary to the initial assumption.
         `zend_free_internal_arg_info()` later `free()`s that exact
         malloc()'d pointer once per owning `zend_internal_function`
         struct. Since the clone is a raw byte-copy, it shared that same
         malloc()'d pointer with the original -> a second, independent
         double-free. Fixed with a new `jsonk_deep_copy_arg_info()`
         helper in `jsonk.c` (plain `malloc()`/`memcpy()`, matching what
         will eventually `free()` it -- not `emalloc`/`pemalloc`), giving
         each clone its own independent copy of the array. Verified safe
         for this codebase specifically because both functions'
         `arg_info` entries are plain scalar types (`IS_MIXED`/`IS_LONG`/
         `_IS_BOOL`/`IS_STRING`, no class-name references) -- a byte copy
         of a `zend_type` holding a class `zend_string*` would need its
         own refcount handling this doesn't attempt.
      Verified fixed by rebuilding in the Docker container and re-running
      the exact reproduction command (`json_encode`/`json_decode`/
      `json_last_error()`/`JSON_THROW_ON_ERROR` all correct, "done, no
      crash" now really means no crash, exit code 0) -- plus the full
      `test1.php`/`test2.php` regression suite (28+28 checks, all still
      passing) and the benchmark (numbers unchanged from decision 17).
    - **phpinfo() now reports pcre/curl/apcu status and the replacement's
      own enabled/disabled state** (user: "Dans le PHPinfo tu metteras
      s'il utilise curl, pcre et apcu" + "Il faudra aussi mettre dans le
      phpinfo si le remplacement de json est désactivé ou activé"):
      `pcre` always "enabled" (hard module dependency, can't actually be
      missing), `curl` via the already-existing `jsonk_curl_available()`
      (made non-static for this), `apcu` via a new public
      `jsonk_apcu_available()`, and the replacement row reading
      `JSONK_G(replace_json_functions)` directly. `DISPLAY_INI_ENTRIES()`
      added right after, so `jsonk.replace_json_functions`'s current
      value is visible too.
    - **Direct answer to "does it support PHP's JSON_* flags reasonably
      well?"**: encode supports `JSON_FORCE_OBJECT`,
      `JSON_PRESERVE_ZERO_FRACTION`, `JSON_NUMERIC_CHECK`,
      `JSON_PARTIAL_OUTPUT_ON_ERROR`, `JSON_UNESCAPED_SLASHES`,
      `JSON_UNESCAPED_UNICODE`, `JSON_PRETTY_PRINT`,
      `JSON_THROW_ON_ERROR`; decode supports `JSON_BIGINT_AS_STRING`,
      `JSON_OBJECT_AS_ARRAY`, `JSON_THROW_ON_ERROR`. Confirmed gaps,
      unchanged from decision 5's original list (item 6 below):
      `JSON_HEX_*` (four flags), `JSON_UNESCAPED_LINE_TERMINATORS`,
      `JSON_INVALID_UTF8_IGNORE`/`_SUBSTITUTE` -- none implemented yet,
      not touched this session.

20. **`config.m4`'s `vendor/simdjson/simdjson.h`/`vendor/yyjson/yyjson.h`
    existence check (and its `PHP_ADD_INCLUDE`/`PHP_NEW_EXTENSION` source
    paths) are relative to the wrong directory when jsonk is built
    statically inside a full `php-src` tree, not standalone via `phpize`
    (found in `php-wasm-compiler`, 2026-09-16, not fixed here yet --
    documentation only, per the user's explicit ask: "documente le truc
    dans son repo").**
    - **Root cause**: `config.m4`'s body (`if test ! -f
      "vendor/simdjson/simdjson.h"; then AC_MSG_ERROR(...); fi`, decision
      11) is a bare shell path, correct for this repo's own documented
      native build (`vendor/build/stage.sh` then `phpize && ./configure
      --enable-jsonk` -- run *from `php-jsonk`'s own directory*, which is
      also the generated `configure` script's cwd, so the relative path
      resolves against `vendor/simdjson/simdjson.h` right there). But
      `php-wasm-compiler` builds PHP statically by dropping `ext/jsonk`'s
      whole source tree into a full `php-src` checkout and running ONE
      `./configure` generated for the *entire tree*, from `php-src`'s own
      root -- so at the point config.m4's inlined shell body actually
      executes, cwd is `php-src/`, not `php-src/ext/jsonk/`, and the same
      bare relative path resolves to `php-src/vendor/simdjson/simdjson.h`
      instead. `php-wasm-compiler` had faithfully vendored the files at
      `ext/jsonk/vendor/simdjson/` (mirroring this repo's own
      `vendor/build/stage.sh` layout) and hit `configure: error: simdjson
      not found at vendor/simdjson/simdjson.h -- run vendor/build/stage.sh
      first` as a result -- a real build failure, not a hypothetical.
    - **Workaround actually shipped, in `php-wasm-compiler` only**: after
      fetching `ext/jsonk/vendor/{simdjson,yyjson}/`, it also copies both
      directories to `php-src/vendor/{simdjson,yyjson}/` (the full-tree
      build root), satisfying the guard check from whichever cwd it
      actually runs at. See `php-wasm-compiler`'s `compile/php/Dockerfile`
      (the jsonk fetch block) and its own `CLAUDE.md` decision 43 for the
      full writeup on that side. Untested here: whether
      `PHP_ADD_INCLUDE([vendor/simdjson])` and `PHP_NEW_EXTENSION`'s
      `vendor/simdjson/simdjson.cpp vendor/yyjson/yyjson.c` source
      arguments would have hit the *same* wrong-cwd problem for the actual
      compile step (not just the guard) had the workaround only fixed the
      header check -- copying both full directories (not just the two
      `.h` files) sidesteps needing to know for sure.
    - **Not fixed here**: the honest, root-cause fix would be for
      `config.m4` to resolve these three references against `$ext_srcdir`
      (the standard PHP build variable holding this extension's own
      source directory, correct regardless of whether the extension is
      built standalone or as part of a bigger tree) instead of a bare
      relative path -- e.g. `test -f "$ext_srcdir/vendor/simdjson/
      simdjson.h"`, `PHP_ADD_INCLUDE([$ext_srcdir/vendor/simdjson])`, and
      prefixing the two vendored source files in the `PHP_NEW_EXTENSION`
      call the same way. That would make `php-wasm-compiler`'s
      root-directory-copy workaround unnecessary, but changes this
      extension's own native build contract (untested since decision
      17's build) -- left as a follow-up, not attempted in this pass.

21. **Real fix, this time in `config.m4` itself: simdjson needs
    `-msimd128` under Emscripten, or its own architecture detection reaches
    real x86 SSE intrinsics that don't exist there (found in
    `php-wasm-compiler`, 2026-09-16, fixed here per the user's explicit
    "va modifier le source directement... et commit/tag").**
    - **Symptom**: `php-wasm-compiler`'s build (with decision 20's vendor-
      path fix applied) got past `./configure` and into actually compiling
      `jsonk_decode.cpp`/`vendor/simdjson/simdjson.cpp`, then failed:
      `.../compat/emmintrin.h:11: error: "SSE2 instruction set not
      enabled"` and the same for `xmmintrin.h`'s SSE guard.
    - **Root cause**: simdjson.h's own preprocessor architecture detection
      sees `__x86_64__` -- defined pipeline-wide by `php-wasm-compiler`
      purely to make `zend_long` 64-bit under Emscripten (a real x86_64
      macro being repurposed for an unrelated ABI reason, not a claim the
      target is real x86 hardware) -- and picks a genuine x86 SIMD backend,
      pulling in `<emmintrin.h>`/`<xmmintrin.h>`. Emscripten ships compat
      shims for exactly these headers (translating SSE-family intrinsics to
      real wasm SIMD128 instructions), but they guard themselves behind
      `__SSE__`/`__SSE2__` (confirmed by reading Emscripten's own
      `compat/xmmintrin.h` from inside a throwaway container built from
      `php-wasm-compiler`'s own base image: `#ifndef __SSE__ #error ...`),
      which clang only predefines once `-msimd128` is passed on the command
      line -- never implied by `-D__x86_64__` alone.
    - **Fix**: `config.m4` now appends `-msimd128` to `CXXFLAGS`, but only
      when `$CXX` matches `*em++*` (a `case` guard) -- the flag is
      Emscripten-specific and would be a hard error on a native, non-Wasm
      build, so it must not leak into this extension's own primary native
      PECL build target (decision 3).
    - **Not verified end-to-end yet**: no native build was re-run this
      session to confirm the `case $CXX in *em++*)` guard doesn't
      misbehave (e.g. if some native `$CXX` value could ever coincidentally
      contain the substring `em++` -- considered and dismissed as
      practically impossible, but genuinely unverified); the real
      confirmation is the next `php-wasm-compiler` build actually compiling
      `simdjson.cpp`/`jsonk_decode.cpp` clean, still in progress as of this
      writing.
    - **Correction, same session**: that first `-msimd128`-only attempt
      was insufficient, confirmed by an actual `php-wasm-compiler` build —
      it only satisfied `emmintrin.h`'s own `__SSE__` guard, then hit
      `<x86intrin.h>`'s further sub-includes (`ia32intrin.h`, AMD-only
      `ammintrin.h`) needing real x86 CPUID/RDTSC/SSE4a builtins with no
      wasm translation at all, confirmed by directly testing `em++
      -msimd128 -msse4.2 -include x86intrin.h` in a throwaway container.
      Real fix: disable simdjson's x86-specific implementations outright
      (`SIMDJSON_IMPLEMENTATION_ICELAKE/HASWELL/WESTMERE=0`, forcing
      `SIMDJSON_BUILTIN_IMPLEMENTATION` to resolve to the portable
      `fallback` on its own) plus `SIMDJSON_EXPERIMENTAL_HAS_SSE2=0`
      (`simdjson.h`'s own separate, `#ifndef`-guarded SSE2 code path).
      Verified directly: a standalone `em++ -c simdjson.cpp`/`#include
      <simdjson.h>` test with just these four defines compiles clean.
      **One remaining piece needed patching simdjson.cpp itself** (not
      overridable via any -D, since `detect_supported_architectures()`'s
      x86 branch is gated by a raw, unguarded `#elif defined(__x86_64__)`
      that re-`#define`s `SIMDJSON_IS_X86_64 1` regardless of any prior
      command-line value) — real x86 `cpuid`/`xgetbv` inline asm, genuinely
      impossible under Emscripten. Since this repo doesn't vendor
      simdjson.cpp itself (`php-wasm-compiler` downloads it directly), that
      one-line patch (adding `&& !defined(__EMSCRIPTEN__)`) lives in
      `php-wasm-compiler`'s own `patches/simdjson/` instead — see that
      repo's CLAUDE.md for the full writeup. Version bumped to `0.1.2`.

22. **`enum`/`const`/`uniqueItems` compare objects by value (0.1.5,
    2026-09-23).** Found while switching php-prepros' `SCHEMA` class to
    jsonk: `{"const":{"x":1}}` rejected `{"x":1}`. Two causes:
    `jsonk_values_equal()` had no `IS_OBJECT` case (so two stdClass values
    were never equal, which also broke `uniqueItems` duplicate detection
    for objects), and `yyjson_val_to_zval()` materialized the schema's
    object literals as PHP arrays while the validated value is a stdClass
    by default. Fix: literals become stdClass (property names always
    string keys), objects compare by property table, and an object vs an
    associative array compare as maps with string/integer key fallback
    (`jsonk_table_find()`), since `JSON_OBJECT_AS_ARRAY` decoding and
    `jsonk_encode()` of a PHP array produce arrays. A non-empty list array
    never equals an object, so `[1,2]` doesn't match `{"0":1,"1":2}`; the
    cost is that an assoc-decoded object keyed exactly `"0"`…`"n"` (a PHP
    list) doesn't match its object literal either. Verified natively
    against PHP 8.5.4 (headers and CLI extracted from Ubuntu's
    `php8.5-dev`/`php8.5-cli` .debs into /tmp, gcc 15 likewise, no
    install), plus php-prepros' SCHEMA differential test.
