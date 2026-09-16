#ifndef JSONK_ERROR_H
#define JSONK_ERROR_H

#include "php_jsonk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Clears error_code/message and empties the violations list. Called at the
 * start of every jsonk_decode()/jsonk_encode()/jsonk_validate() so state
 * never leaks from one call to the next. */
void jsonk_reset_errors(void);

/* Sets a single, non-schema error (JSON syntax, depth, unsupported type,
 * recursion, or a malformed $schema argument itself). Overwrites any
 * previous error_code/message but never touches the violations list. */
void jsonk_set_error(int code, const char *format, ...);

/* Appends one schema violation (a JSON-Pointer-style `path`, the offending
 * keyword, and a human-readable message) to jsonk_get_last_errors()'s
 * backing array, and sets error_code to JSONK_ERROR_SCHEMA_VIOLATION unless
 * it's already JSONK_ERROR_SCHEMA_INVALID (a broken schema always wins --
 * the violation list is meaningless if the schema itself couldn't be
 * compiled). `path` empty or NULL means the document root. */
void jsonk_add_violation(const char *path, const char *keyword, const char *format, ...);

/* True if at least one violation was recorded since the last
 * jsonk_reset_errors(). */
bool jsonk_has_violations(void);

/* anyOf/oneOf/not need to probe a branch and look only at the boolean
 * result, without polluting jsonk_get_last_errors() with "tried this
 * branch and it didn't match" noise. Nestable (a suppressed probe may
 * itself contain another combinator). */
void jsonk_suppress_violations_push(void);
void jsonk_suppress_violations_pop(void);

#ifdef __cplusplus
}
#endif

#endif /* JSONK_ERROR_H */
