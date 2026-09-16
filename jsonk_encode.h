#ifndef JSONK_ENCODE_H
#define JSONK_ENCODE_H

#include "php_jsonk.h"
#include "jsonk_schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Validates `value` against `schema` (skipped when NULL) and serializes it
 * to JSON via yyjson. Returns an owned zend_string on success, or NULL on
 * failure (schema violation, unsupported PHP type, non-finite float, or
 * object recursion) -- check JSONK_G(error_code)/JSONK_G(error_message) /
 * jsonk_get_last_errors(). `flags` reuses ext/json's PHP_JSON_* macros
 * (see jsonk.c). */
zend_string *jsonk_encode_impl(zval *value, jsonk_schema_node *schema, zend_long flags, zend_long depth);

#ifdef __cplusplus
}
#endif

#endif /* JSONK_ENCODE_H */
