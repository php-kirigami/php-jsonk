#ifndef JSONK_REGEX_H
#define JSONK_REGEX_H

#include "php_jsonk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reuses PHP's own bundled PCRE2 (ext/pcre, a hard module dependency --
 * see jsonk.c/config.m4) instead of vendoring a regex engine: PHP has
 * required pcre since 7.3 and can't be built without it, so this adds
 * zero new dependencies. Used for the "pattern"/"patternProperties"
 * schema keywords and the "regex" format check.
 *
 * Known, documented limitation: JSON Schema specifies ECMA-262
 * (JavaScript) regex syntax; PCRE is not byte-for-byte identical (mostly
 * around obscure Unicode property escape names and some lookbehind
 * edge cases). Ordinary character classes/anchors/quantifiers/groups --
 * what the overwhelming majority of real "pattern" values actually use --
 * behave the same under both. */

bool jsonk_regex_match(const char *pattern, size_t pattern_len, const char *subject, size_t subject_len);

/* Used for format:"regex" -- does `pattern` itself compile as a regex? */
bool jsonk_regex_is_valid(const char *pattern, size_t pattern_len);

#ifdef __cplusplus
}
#endif

#endif /* JSONK_REGEX_H */
