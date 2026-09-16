#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_jsonk.h"
#include "jsonk_regex.h"
#include "ext/pcre/php_pcre.h"
#include <string.h>

/* Wraps a bare ECMA-262 regex body (JSON Schema's "pattern" has no
 * delimiters) in PCRE delimiters. \x01 is used as the delimiter -- a real
 * regex pattern essentially never contains that raw control byte, so this
 * avoids scanning for/escaping a printable delimiter that might collide
 * with pattern content. The trailing 'u' runs PCRE in UTF-8 mode, matching
 * JSON's own string semantics. */
static zend_string *wrap_pattern(const char *pattern, size_t pattern_len)
{
	size_t wrapped_len = pattern_len + 3;
	zend_string *wrapped = zend_string_alloc(wrapped_len, 0);
	char *buf = ZSTR_VAL(wrapped);

	buf[0] = '\x01';
	memcpy(buf + 1, pattern, pattern_len);
	buf[1 + pattern_len] = '\x01';
	buf[2 + pattern_len] = 'u';
	buf[3 + pattern_len] = '\0';

	return wrapped;
}

/* pcre_get_compiled_regex_cache() emits a PHP E_WARNING on a malformed
 * pattern (the same as a broken preg_match() pattern would) -- this is
 * PHP's own behavior for that function, not silenced here. A schema
 * compiled from a genuinely invalid "pattern"/"patternProperties" regex
 * will therefore surface as a warning in addition to a compile-time
 * JSONK_ERROR_SCHEMA_INVALID (see jsonk_schema.c). */
static pcre2_code *compile_pattern(const char *pattern, size_t pattern_len)
{
	zend_string *wrapped = wrap_pattern(pattern, pattern_len);
	pcre_cache_entry *pce = pcre_get_compiled_regex_cache(wrapped);
	zend_string_release(wrapped);

	return pce ? php_pcre_pce_re(pce) : NULL;
}

bool jsonk_regex_match(const char *pattern, size_t pattern_len, const char *subject, size_t subject_len)
{
	pcre2_code *re;
	pcre2_match_data *match_data;
	int rc;

	re = compile_pattern(pattern, pattern_len);
	if (!re) {
		return false;
	}

	match_data = php_pcre_create_match_data(0, re);
	if (!match_data) {
		return false;
	}

	rc = pcre2_match(re, (PCRE2_SPTR) subject, (PCRE2_SIZE) subject_len, 0, 0, match_data, php_pcre_mctx());
	php_pcre_free_match_data(match_data);

	return rc >= 0;
}

bool jsonk_regex_is_valid(const char *pattern, size_t pattern_len)
{
	return compile_pattern(pattern, pattern_len) != NULL;
}
