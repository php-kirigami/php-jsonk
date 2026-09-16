/*
  +----------------------------------------------------------------------+
  | php-jsonk                                                             |
  +----------------------------------------------------------------------+
  | Copyright (c) Maxime Larrivée-Roy                                     |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2 of the GNU General Public   |
  | License, that is bundled with this package in the file LICENSE.      |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_JSONK_H
#define PHP_JSONK_H

extern zend_module_entry jsonk_module_entry;
#define phpext_jsonk_ptr &jsonk_module_entry

#define PHP_JSONK_VERSION "0.1.2"

#ifdef PHP_WIN32
# define PHP_JSONK_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
# define PHP_JSONK_API __attribute__ ((visibility("default")))
#else
# define PHP_JSONK_API
#endif

#ifdef ZTS
#include "TSRM.h"
#endif

PHP_MINIT_FUNCTION(jsonk);
PHP_MSHUTDOWN_FUNCTION(jsonk);
PHP_RINIT_FUNCTION(jsonk);
PHP_RSHUTDOWN_FUNCTION(jsonk);
PHP_MINFO_FUNCTION(jsonk);

/* error codes -- mirrors ext/json's own php_json_error_code shape (an int
 * enum plus jsonk_last_error()/jsonk_last_error_msg() accessors), but a
 * distinct namespace since these describe simdjson/yyjson/schema failures,
 * not ext/json's own parser. */
typedef enum {
	JSONK_ERROR_NONE = 0,
	JSONK_ERROR_SYNTAX,             /* malformed JSON input */
	JSONK_ERROR_DEPTH,              /* nesting deeper than the $depth argument */
	JSONK_ERROR_UTF8,               /* invalid UTF-8 in the JSON input */
	JSONK_ERROR_SCHEMA_INVALID,     /* the $schema argument itself isn't usable */
	JSONK_ERROR_SCHEMA_VIOLATION,   /* value doesn't conform -- see jsonk_get_last_errors() */
	JSONK_ERROR_UNSUPPORTED_TYPE,   /* encode: a PHP value jsonk can't serialize */
	JSONK_ERROR_RECURSION,          /* encode: circular reference */
} jsonk_error_code;

extern zend_class_entry *jsonk_exception_ce;

ZEND_BEGIN_MODULE_GLOBALS(jsonk)
	int error_code;
	zend_string *error_message; /* owned; NULL when error_code == JSONK_ERROR_NONE */
	zval last_errors;           /* always a valid IS_ARRAY zval between RINIT/RSHUTDOWN */
	int suppress_violations;    /* >0 while probing anyOf/oneOf/not branches -- see jsonk_error.c */
	bool replace_json_functions; /* "jsonk.replace_json_functions" ini setting, PHP_INI_SYSTEM -- see jsonk.c's MINIT */
ZEND_END_MODULE_GLOBALS(jsonk)

ZEND_EXTERN_MODULE_GLOBALS(jsonk)
#define JSONK_G(v) ZEND_MODULE_GLOBALS_ACCESSOR(jsonk, v)

#endif /* PHP_JSONK_H */
