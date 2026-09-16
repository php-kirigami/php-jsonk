#ifndef JSONK_FORMAT_H
#define JSONK_FORMAT_H

#include "php_jsonk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Checks `value` against the JSON Schema "format" name `format`/`format_len`
 * (e.g. "date-time", "email", "uuid", ...). Per the JSON Schema spec,
 * "format" is annotation-only unless a validator chooses to also assert
 * it -- jsonk asserts the common ones (see jsonk_format.c for the full
 * list and what's pragmatic/simplified vs. exact). An UNRECOGNIZED format
 * name always returns true (no assertion), matching the spec's own
 * default behavior for formats a validator doesn't implement. */
bool jsonk_check_format(const char *format, size_t format_len, const char *value, size_t value_len);

#ifdef __cplusplus
}
#endif

#endif /* JSONK_FORMAT_H */
