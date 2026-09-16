#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_jsonk.h"
#include "jsonk_error.h"
#include <stdarg.h>

static void jsonk_replace_error_message(zend_string *msg)
{
	if (JSONK_G(error_message)) {
		zend_string_release(JSONK_G(error_message));
	}
	JSONK_G(error_message) = msg;
}

void jsonk_reset_errors(void)
{
	JSONK_G(error_code) = JSONK_ERROR_NONE;
	jsonk_replace_error_message(NULL);
	zend_hash_clean(Z_ARRVAL(JSONK_G(last_errors)));
}

void jsonk_set_error(int code, const char *format, ...)
{
	va_list args;
	zend_string *msg;

	va_start(args, format);
	msg = vstrpprintf(0, format, args);
	va_end(args);

	JSONK_G(error_code) = code;
	jsonk_replace_error_message(msg);
}

bool jsonk_has_violations(void)
{
	return zend_hash_num_elements(Z_ARRVAL(JSONK_G(last_errors))) > 0;
}

void jsonk_suppress_violations_push(void)
{
	JSONK_G(suppress_violations)++;
}

void jsonk_suppress_violations_pop(void)
{
	JSONK_G(suppress_violations)--;
}

void jsonk_add_violation(const char *path, const char *keyword, const char *format, ...)
{
	va_list args;
	zend_string *msg;
	zval entry;

	if (JSONK_G(suppress_violations) > 0) {
		return;
	}

	va_start(args, format);
	msg = vstrpprintf(0, format, args);
	va_end(args);

	array_init(&entry);
	add_assoc_string(&entry, "path", (path && *path) ? path : "/");
	add_assoc_string(&entry, "keyword", keyword);
	add_assoc_str(&entry, "message", msg);

	add_next_index_zval(&JSONK_G(last_errors), &entry);

	/* A malformed schema (JSONK_ERROR_SCHEMA_INVALID) always takes priority
	 * over violations found before the invalidity was noticed -- the list
	 * is meaningless once that happens. */
	if (JSONK_G(error_code) != JSONK_ERROR_SCHEMA_INVALID) {
		JSONK_G(error_code) = JSONK_ERROR_SCHEMA_VIOLATION;
		jsonk_replace_error_message(zend_string_init(
			ZEND_STRL("Value does not match schema; see jsonk_get_last_errors()"), 0));
	}
}
