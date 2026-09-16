#ifndef JSONK_DECODE_H
#define JSONK_DECODE_H

#include "php_jsonk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Parses `json`/`json_len` via simdjson's DOM API and builds the resulting
 * PHP value into `return_value`. Schema validation is NOT done here -- see
 * CLAUDE.md "Decode/encode + schema: two passes, not fused (v1)": the
 * caller (jsonk.c) runs jsonk_schema_validate_zval() on the result
 * afterward when a $schema was given, reusing the same validator the
 * encode path uses instead of a second, DOM-walking schema engine.
 *
 * `flags` is interpreted for PHP_JSON_OBJECT_AS_ARRAY and
 * PHP_JSON_BIGINT_AS_STRING (ext/json's own macros, reused as-is -- see
 * jsonk.c). Returns 1 on success, 0 on failure (check
 * JSONK_G(error_code)/JSONK_G(error_message), set via jsonk_set_error()). */
int jsonk_decode_impl(const char *json, size_t json_len, zend_long flags, zend_long depth, zval *return_value);

#ifdef __cplusplus
}
#endif

#endif /* JSONK_DECODE_H */
