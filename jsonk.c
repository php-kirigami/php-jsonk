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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "zend_exceptions.h"
#include "zend_smart_str.h"
#include "zend_modules.h"
#include "ext/json/php_json.h"

#include "php_jsonk.h"
#include "jsonk_error.h"
#include "jsonk_schema.h"
#include "jsonk_decode.h"
#include "jsonk_encode.h"
#include "jsonk_fetch.h"

ZEND_DECLARE_MODULE_GLOBALS(jsonk)

zend_class_entry *jsonk_exception_ce;

static const zend_module_dep jsonk_deps[] = {
	ZEND_MOD_REQUIRED("json")
	ZEND_MOD_REQUIRED("pcre") /* "pattern"/"patternProperties"/format:"regex" -- see jsonk_regex.c */
	ZEND_MOD_OPTIONAL("curl") /* external "$ref" resolution -- see jsonk_fetch.c; everything else works fine without it */
	ZEND_MOD_END
};

/* ========================================================================
 * JsonkException
 * ======================================================================== */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_jsonk_exception_get_errors, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_METHOD(JsonkException, getErrors)
{
	zval *errors;

	ZEND_PARSE_PARAMETERS_NONE();

	errors = zend_read_property(jsonk_exception_ce, Z_OBJ_P(ZEND_THIS), "errors", sizeof("errors") - 1, 1, NULL);
	ZVAL_COPY(return_value, errors);
}

static const zend_function_entry jsonk_exception_methods[] = {
	PHP_ME(JsonkException, getErrors, arginfo_jsonk_exception_get_errors, ZEND_ACC_PUBLIC)
	PHP_FE_END
};

static void jsonk_throw_exception(void)
{
	zend_object *exc;
	zval errors_copy;

	exc = zend_throw_exception(
		jsonk_exception_ce,
		JSONK_G(error_message) ? ZSTR_VAL(JSONK_G(error_message)) : "Unknown jsonk error",
		JSONK_G(error_code)
	);

	ZVAL_COPY(&errors_copy, &JSONK_G(last_errors));
	zend_update_property(jsonk_exception_ce, exc, "errors", sizeof("errors") - 1, &errors_copy);
	zval_ptr_dtor(&errors_copy);
}

/* ========================================================================
 * jsonk_decode() / jsonk_encode() / jsonk_validate()
 * ======================================================================== */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_jsonk_decode, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, json, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, schema, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, associative, _IS_BOOL, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, depth, IS_LONG, 0, "512")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, flags, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

PHP_FUNCTION(jsonk_decode)
{
	zend_string *json;
	zend_string *schema_str = NULL;
	bool associative = 0;
	bool associative_is_null = 1;
	zend_long depth = 512;
	zend_long flags = 0;
	jsonk_schema_node *schema = NULL;
	bool assoc;
	zend_long decode_flags;
	bool ok = true;

	ZEND_PARSE_PARAMETERS_START(1, 5)
		Z_PARAM_STR(json)
		Z_PARAM_OPTIONAL
		Z_PARAM_STR_OR_NULL(schema_str)
		Z_PARAM_BOOL_OR_NULL(associative, associative_is_null)
		Z_PARAM_LONG(depth)
		Z_PARAM_LONG(flags)
	ZEND_PARSE_PARAMETERS_END();

	jsonk_reset_errors();
	ZVAL_NULL(return_value);

	if (depth <= 0) {
		jsonk_set_error(JSONK_ERROR_DEPTH, "Depth must be greater than zero");
		ok = false;
	}

	if (ok && schema_str) {
		schema = jsonk_schema_compile(ZSTR_VAL(schema_str), ZSTR_LEN(schema_str));
		ok = (schema != NULL);
	}

	if (ok) {
		assoc = associative_is_null ? ((flags & PHP_JSON_OBJECT_AS_ARRAY) != 0) : associative;
		decode_flags = assoc ? (flags | PHP_JSON_OBJECT_AS_ARRAY) : (flags & ~(zend_long) PHP_JSON_OBJECT_AS_ARRAY);
		ok = jsonk_decode_impl(ZSTR_VAL(json), ZSTR_LEN(json), decode_flags, depth, return_value) != 0;
	}

	if (ok && schema) {
		smart_str path = {0};
		ok = jsonk_schema_validate_zval(schema, return_value, &path, depth, NULL, NULL);
		smart_str_free(&path);
		if (!ok) {
			zval_ptr_dtor(return_value);
		}
	}

	if (schema) {
		jsonk_schema_free(schema);
	}

	if (!ok) {
		ZVAL_NULL(return_value);
		if (flags & PHP_JSON_THROW_ON_ERROR) {
			jsonk_throw_exception();
			RETURN_THROWS();
		}
	}
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_jsonk_encode, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, schema, IS_STRING, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, flags, IS_LONG, 0, "0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, depth, IS_LONG, 0, "512")
ZEND_END_ARG_INFO()

PHP_FUNCTION(jsonk_encode)
{
	zval *value;
	zend_string *schema_str = NULL;
	zend_long flags = 0;
	zend_long depth = 512;
	jsonk_schema_node *schema = NULL;
	zend_string *result = NULL;
	bool ok = true;

	ZEND_PARSE_PARAMETERS_START(1, 4)
		Z_PARAM_ZVAL(value)
		Z_PARAM_OPTIONAL
		Z_PARAM_STR_OR_NULL(schema_str)
		Z_PARAM_LONG(flags)
		Z_PARAM_LONG(depth)
	ZEND_PARSE_PARAMETERS_END();

	jsonk_reset_errors();

	if (depth <= 0) {
		jsonk_set_error(JSONK_ERROR_DEPTH, "Depth must be greater than zero");
		ok = false;
	}

	if (ok && schema_str) {
		schema = jsonk_schema_compile(ZSTR_VAL(schema_str), ZSTR_LEN(schema_str));
		ok = (schema != NULL);
	}

	if (ok) {
		result = jsonk_encode_impl(value, schema, flags, depth);
		ok = (result != NULL);
	}

	if (schema) {
		jsonk_schema_free(schema);
	}

	if (!ok) {
		if (flags & PHP_JSON_THROW_ON_ERROR) {
			jsonk_throw_exception();
			RETURN_THROWS();
		}
		RETURN_FALSE;
	}

	RETURN_STR(result);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_jsonk_validate, 0, 2, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, json, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, schema, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, depth, IS_LONG, 0, "512")
ZEND_END_ARG_INFO()

PHP_FUNCTION(jsonk_validate)
{
	zend_string *json;
	zend_string *schema_str;
	zend_long depth = 512;
	jsonk_schema_node *schema;
	zval decoded;
	bool ok;

	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_STR(json)
		Z_PARAM_STR(schema_str)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(depth)
	ZEND_PARSE_PARAMETERS_END();

	jsonk_reset_errors();

	if (depth <= 0) {
		jsonk_set_error(JSONK_ERROR_DEPTH, "Depth must be greater than zero");
		RETURN_FALSE;
	}

	schema = jsonk_schema_compile(ZSTR_VAL(schema_str), ZSTR_LEN(schema_str));
	if (!schema) {
		RETURN_FALSE;
	}

	ok = jsonk_decode_impl(ZSTR_VAL(json), ZSTR_LEN(json), 0, depth, &decoded) != 0;
	if (ok) {
		smart_str path = {0};
		ok = jsonk_schema_validate_zval(schema, &decoded, &path, depth, NULL, NULL);
		smart_str_free(&path);
		zval_ptr_dtor(&decoded);
	}
	jsonk_schema_free(schema);

	RETURN_BOOL(ok);
}

/* ========================================================================
 * Error introspection
 * ======================================================================== */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_jsonk_last_error, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(jsonk_last_error)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_LONG(JSONK_G(error_code));
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_jsonk_last_error_msg, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(jsonk_last_error_msg)
{
	ZEND_PARSE_PARAMETERS_NONE();
	if (JSONK_G(error_message)) {
		RETURN_STR_COPY(JSONK_G(error_message));
	}
	RETURN_STRING("No error");
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_jsonk_get_last_errors, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

PHP_FUNCTION(jsonk_get_last_errors)
{
	ZEND_PARSE_PARAMETERS_NONE();
	RETURN_ZVAL(&JSONK_G(last_errors), 1, 0);
}

/* ========================================================================
 * Optional replacement of the native json_encode()/json_decode() --
 * gated behind the "jsonk.replace_json_functions" ini setting (off by
 * default), see PHP_MINIT_FUNCTION(jsonk) below for how the actual
 * function-table swap happens. These two functions match native
 * json_encode()/json_decode()'s exact signature (no $schema -- that
 * stays jsonk_encode()/jsonk_decode()'s own thing, per the user's
 * explicit clarification the day this was scoped) and are otherwise
 * thin wrappers around the same jsonk_encode_impl()/jsonk_decode_impl()
 * engines jsonk_encode()/jsonk_decode() use. Registered under their own
 * name here (not "json_encode"/"json_decode" directly -- PHP's normal
 * registration path can't declare two extensions' functions under the
 * same name) purely so PHP's own zend_register_functions() builds a
 * fully-formed zend_internal_function for us; MINIT then clones that
 * struct into the "json_encode"/"json_decode" slots of the function
 * table when the ini setting is on. Also directly callable under their
 * own name regardless (a harmless, undocumented alias -- there's no way
 * to register a function without that also being true).
 *
 * Sets ext/json's OWN error state (JSON_G(error_code), via the mapping
 * below) rather than jsonk's -- existing code calling json_last_error()/
 * json_last_error_msg() after a call to the (now jsonk-backed)
 * json_encode()/json_decode() keeps working as expected. jsonk's own
 * jsonk_last_error()/jsonk_get_last_errors() are deliberately left
 * untouched by these two -- they describe jsonk_encode()/jsonk_decode()
 * calls specifically, not this replacement path.
 * ======================================================================== */

/* jsonk's error codes are a superset of ext/json's own (schema-specific
 * codes have no ext/json equivalent, but can't occur here anyway since
 * these wrappers never pass a $schema). */
static php_json_error_code jsonk_error_to_php_json_error(int code)
{
	switch (code) {
		case JSONK_ERROR_NONE:             return PHP_JSON_ERROR_NONE;
		case JSONK_ERROR_SYNTAX:           return PHP_JSON_ERROR_SYNTAX;
		case JSONK_ERROR_DEPTH:            return PHP_JSON_ERROR_DEPTH;
		case JSONK_ERROR_UTF8:             return PHP_JSON_ERROR_UTF8;
		case JSONK_ERROR_RECURSION:        return PHP_JSON_ERROR_RECURSION;
		case JSONK_ERROR_UNSUPPORTED_TYPE: return PHP_JSON_ERROR_UNSUPPORTED_TYPE;
		default:                           return PHP_JSON_ERROR_SYNTAX;
	}
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_jsonk_json_encode_replacement, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, value, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, flags, IS_LONG, 0, "0")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, depth, IS_LONG, 0, "512")
ZEND_END_ARG_INFO()

PHP_FUNCTION(jsonk_json_encode_replacement)
{
	zval *value;
	zend_long flags = 0;
	zend_long depth = 512;
	zend_string *result;

	ZEND_PARSE_PARAMETERS_START(1, 3)
		Z_PARAM_ZVAL(value)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(flags)
		Z_PARAM_LONG(depth)
	ZEND_PARSE_PARAMETERS_END();

	if (depth <= 0) {
		JSON_G(error_code) = PHP_JSON_ERROR_DEPTH;
		if (flags & PHP_JSON_THROW_ON_ERROR) {
			zend_throw_exception(jsonk_exception_ce, "Depth must be greater than zero", PHP_JSON_ERROR_DEPTH);
			RETURN_THROWS();
		}
		RETURN_FALSE;
	}

	result = jsonk_encode_impl(value, NULL, flags, depth);
	if (!result) {
		JSON_G(error_code) = jsonk_error_to_php_json_error(JSONK_G(error_code));
		if (flags & PHP_JSON_THROW_ON_ERROR) {
			zend_throw_exception(jsonk_exception_ce, JSONK_G(error_message) ? ZSTR_VAL(JSONK_G(error_message)) : "Unknown error", JSON_G(error_code));
			RETURN_THROWS();
		}
		RETURN_FALSE;
	}

	JSON_G(error_code) = PHP_JSON_ERROR_NONE;
	RETURN_STR(result);
}

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_jsonk_json_decode_replacement, 0, 1, IS_MIXED, 0)
	ZEND_ARG_TYPE_INFO(0, json, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, associative, _IS_BOOL, 1, "null")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, depth, IS_LONG, 0, "512")
	ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, flags, IS_LONG, 0, "0")
ZEND_END_ARG_INFO()

PHP_FUNCTION(jsonk_json_decode_replacement)
{
	zend_string *json;
	bool associative = 0;
	bool associative_is_null = 1;
	zend_long depth = 512;
	zend_long flags = 0;
	bool assoc;
	zend_long decode_flags;
	bool ok;

	ZEND_PARSE_PARAMETERS_START(1, 4)
		Z_PARAM_STR(json)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL_OR_NULL(associative, associative_is_null)
		Z_PARAM_LONG(depth)
		Z_PARAM_LONG(flags)
	ZEND_PARSE_PARAMETERS_END();

	ZVAL_NULL(return_value);

	if (depth <= 0) {
		JSON_G(error_code) = PHP_JSON_ERROR_DEPTH;
		if (flags & PHP_JSON_THROW_ON_ERROR) {
			zend_throw_exception(jsonk_exception_ce, "Depth must be greater than zero", PHP_JSON_ERROR_DEPTH);
			RETURN_THROWS();
		}
		return;
	}

	assoc = associative_is_null ? ((flags & PHP_JSON_OBJECT_AS_ARRAY) != 0) : associative;
	decode_flags = assoc ? (flags | PHP_JSON_OBJECT_AS_ARRAY) : (flags & ~(zend_long) PHP_JSON_OBJECT_AS_ARRAY);

	ok = jsonk_decode_impl(ZSTR_VAL(json), ZSTR_LEN(json), decode_flags, depth, return_value) != 0;
	JSON_G(error_code) = ok ? PHP_JSON_ERROR_NONE : jsonk_error_to_php_json_error(JSONK_G(error_code));

	if (!ok) {
		ZVAL_NULL(return_value);
		if (flags & PHP_JSON_THROW_ON_ERROR) {
			zend_throw_exception(jsonk_exception_ce, JSONK_G(error_message) ? ZSTR_VAL(JSONK_G(error_message)) : "Unknown error", JSON_G(error_code));
			RETURN_THROWS();
		}
	}
}

/* ========================================================================
 * Module plumbing
 * ======================================================================== */

PHP_INI_BEGIN()
	STD_PHP_INI_BOOLEAN("jsonk.replace_json_functions", "0", PHP_INI_SYSTEM, OnUpdateBool, replace_json_functions, zend_jsonk_globals, jsonk_globals)
PHP_INI_END()

static const zend_function_entry jsonk_functions[] = {
	PHP_FE(jsonk_decode, arginfo_jsonk_decode)
	PHP_FE(jsonk_encode, arginfo_jsonk_encode)
	PHP_FE(jsonk_validate, arginfo_jsonk_validate)
	PHP_FE(jsonk_last_error, arginfo_jsonk_last_error)
	PHP_FE(jsonk_last_error_msg, arginfo_jsonk_last_error_msg)
	PHP_FE(jsonk_get_last_errors, arginfo_jsonk_get_last_errors)
	PHP_FE(jsonk_json_encode_replacement, arginfo_jsonk_json_encode_replacement)
	PHP_FE(jsonk_json_decode_replacement, arginfo_jsonk_json_decode_replacement)
	PHP_FE_END
};

/* zend_register_functions() (Zend/zend_API.c) gives any internal function
 * that declares real parameter/return types (ZEND_ACC_HAS_TYPE_HINTS or
 * ZEND_ACC_HAS_RETURN_TYPE -- true for both replacement functions, see
 * their ZEND_ARG_TYPE_INFO() declarations above) a freshly malloc()'d
 * `arg_info` array, replacing the pointer that macro-declared it into a
 * static compiled-in array. zend_free_internal_arg_info() (Zend/zend_
 * opcode.c) later free()s that exact malloc()'d pointer once per
 * zend_internal_function struct that owns it. Our function-table clone in
 * PHP_MINIT_FUNCTION below is a raw byte-copy of an already-registered
 * struct, so without this, the clone and the original both point at the
 * SAME malloc()'d array and both try to free() it at MSHUTDOWN -- a real,
 * confirmed double-free crash. Giving the clone its own independent copy
 * (freed independently, once per owner, exactly like zend_free_internal_
 * arg_info() expects) fixes it. Plain malloc()/memcpy(), not emalloc()/
 * pemalloc() -- the eventual free() is always the libc one. Only safe
 * because our arg_info entries are plain scalar types (IS_MIXED/IS_LONG/
 * _IS_BOOL/IS_STRING, no class-name references) -- a byte copy of a
 * zend_type holding a class zend_string* would need its own refcount
 * handling too, which this does not attempt. */
static void jsonk_deep_copy_arg_info(zend_internal_function *fn)
{
	if ((fn->fn_flags & (ZEND_ACC_HAS_RETURN_TYPE | ZEND_ACC_HAS_TYPE_HINTS)) && fn->arg_info) {
		uint32_t num_args = fn->num_args + 1; /* +1: the return-type slot at index [-1] */
		zend_internal_arg_info *orig = fn->arg_info - 1;
		zend_internal_arg_info *copy;

		if (fn->fn_flags & ZEND_ACC_VARIADIC) {
			num_args++;
		}
		copy = malloc(sizeof(zend_internal_arg_info) * num_args);
		memcpy(copy, orig, sizeof(zend_internal_arg_info) * num_args);
		fn->arg_info = copy + 1;
	}
}

PHP_MINIT_FUNCTION(jsonk)
{
	zend_class_entry ce;
	zend_class_entry *json_exception_ce;

	/* "json" is a hard module dependency (see jsonk_deps below), so its
	 * MINIT -- which registers JsonException -- has already run by the
	 * time ours does. The fallback to zend_ce_exception should therefore
	 * be unreachable; it's there only so a MINIT ordering surprise
	 * degrades to "JsonkException extends Exception" instead of a crash. */
	json_exception_ce = zend_hash_str_find_ptr(CG(class_table), "jsonexception", sizeof("jsonexception") - 1);
	if (!json_exception_ce) {
		json_exception_ce = zend_ce_exception;
	}

	INIT_CLASS_ENTRY(ce, "JsonkException", jsonk_exception_methods);
	jsonk_exception_ce = zend_register_internal_class_ex(&ce, json_exception_ce);
	zend_declare_property_null(jsonk_exception_ce, "errors", sizeof("errors") - 1, ZEND_ACC_PROTECTED);

	REGISTER_LONG_CONSTANT("JSONK_ERROR_NONE", JSONK_ERROR_NONE, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSONK_ERROR_SYNTAX", JSONK_ERROR_SYNTAX, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSONK_ERROR_DEPTH", JSONK_ERROR_DEPTH, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSONK_ERROR_UTF8", JSONK_ERROR_UTF8, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSONK_ERROR_SCHEMA_INVALID", JSONK_ERROR_SCHEMA_INVALID, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSONK_ERROR_SCHEMA_VIOLATION", JSONK_ERROR_SCHEMA_VIOLATION, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSONK_ERROR_UNSUPPORTED_TYPE", JSONK_ERROR_UNSUPPORTED_TYPE, CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("JSONK_ERROR_RECURSION", JSONK_ERROR_RECURSION, CONST_CS | CONST_PERSISTENT);

	jsonk_fetch_cache_init();

	REGISTER_INI_ENTRIES();

	/* "jsonk.replace_json_functions" (PHP_INI_SYSTEM -- fixed for the
	 * process, matching how a function-table structural change has to
	 * behave) clones jsonk's own already-registered
	 * "jsonk_json_encode_replacement"/"jsonk_json_decode_replacement"
	 * zend_internal_function structs (built correctly by PHP's own
	 * zend_register_functions(), just above -- see the doc comment on
	 * those two PHP_FUNCTIONs for why this is safe to do rather than
	 * hand-building a zend_internal_function ourselves) directly into
	 * the "json_encode"/"json_decode" slots of the SAME persistent
	 * function table ext/json's own MINIT already populated (guaranteed
	 * to have already run, per the hard "json" module dependency).
	 * zend_hash_str_update_mem() has update, not add, semantics -- it
	 * replaces the existing entry outright, and returns a pointer to the
	 * newly-stored copy. */
	if (JSONK_G(replace_json_functions)) {
		zend_function *my_encode = zend_hash_str_find_ptr(CG(function_table), ZEND_STRL("jsonk_json_encode_replacement"));
		zend_function *my_decode = zend_hash_str_find_ptr(CG(function_table), ZEND_STRL("jsonk_json_decode_replacement"));
		zend_internal_function *installed;

		if (my_encode) {
			installed = (zend_internal_function *) zend_hash_str_update_mem(CG(function_table), ZEND_STRL("json_encode"), my_encode, sizeof(zend_internal_function));
			/* The byte-copy above left `function_name` AND `arg_info`
			 * pointing at the exact same objects the original
			 * "jsonk_json_encode_replacement" entry still owns --
			 * confirmed as two independent real double-free crashes at
			 * shutdown (both entries' teardown released the same
			 * zend_string, and, separately, zend_free_internal_arg_info()
			 * free()d the same malloc()'d arg_info array twice -- see
			 * jsonk_deep_copy_arg_info()'s doc comment for why that array
			 * even exists to begin with). Give the clone its own,
			 * independently-owned copies of both instead of sharing them
			 * -- the name fix also corrects error messages/reflection
			 * showing the wrong function name. */
			if (installed) {
				installed->function_name = zend_string_init(ZEND_STRL("json_encode"), 1);
				jsonk_deep_copy_arg_info(installed);
			}
		}
		if (my_decode) {
			installed = (zend_internal_function *) zend_hash_str_update_mem(CG(function_table), ZEND_STRL("json_decode"), my_decode, sizeof(zend_internal_function));
			if (installed) {
				installed->function_name = zend_string_init(ZEND_STRL("json_decode"), 1);
				jsonk_deep_copy_arg_info(installed);
			}
		}
	}

	return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(jsonk)
{
	jsonk_fetch_cache_shutdown();
	UNREGISTER_INI_ENTRIES();

	return SUCCESS;
}

PHP_RINIT_FUNCTION(jsonk)
{
	JSONK_G(error_code) = JSONK_ERROR_NONE;
	JSONK_G(error_message) = NULL;
	JSONK_G(suppress_violations) = 0;
	array_init(&JSONK_G(last_errors));
	return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(jsonk)
{
	if (JSONK_G(error_message)) {
		zend_string_release(JSONK_G(error_message));
		JSONK_G(error_message) = NULL;
	}
	zval_ptr_dtor(&JSONK_G(last_errors));
	return SUCCESS;
}

PHP_MINFO_FUNCTION(jsonk)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "jsonk support", "enabled");
	php_info_print_table_row(2, "version", PHP_JSONK_VERSION);
	/* pcre is a hard module dependency (jsonk can't even load without it,
	 * see the zend_module_dep in this file) -- checked for real anyway,
	 * rather than hardcoded "yes", for consistency with the other two
	 * rows and as a sanity check on the dependency actually holding. */
	php_info_print_table_row(2, "pcre (pattern/patternProperties/format:\"regex\")",
		zend_hash_str_exists(&module_registry, ZEND_STRL("pcre")) ? "enabled" : "MISSING (should be impossible)");
	php_info_print_table_row(2, "curl (external \"$ref\" resolution)",
		jsonk_curl_available() ? "enabled" : "not available -- external $ref will fail");
	php_info_print_table_row(2, "apcu (external \"$ref\" fetch cache)",
		jsonk_apcu_available() ? "enabled, used as the fetch cache" : "not active -- using the built-in process-local fallback cache");
	php_info_print_table_row(2, "json_encode()/json_decode() replacement",
		JSONK_G(replace_json_functions) ? "enabled -- native json_encode()/json_decode() are jsonk-backed" : "disabled -- native json_encode()/json_decode() are unmodified");
	/* php_info_print_table_row()/_header() don't escape their arguments,
	 * so a raw two-cell footer row works here too -- same Kirigami logo
	 * used as the README header, kept as a hosted URL rather than a
	 * base64 blob to avoid bloating this binary. Plain <img> also
	 * survives an HTML->Markdown conversion of phpinfo()'s output cleanly
	 * (renders as `![...](url)`), unlike inline <svg> markup, which not
	 * every such converter preserves. */
	php_printf(
		"<tr><td style=\"text-align: left\">Part of the Kirigami PHP extension family</td>"
		"<td style=\"text-align: right\">"
		"<img src=\"https://zmotrin.github.io/assets/kirigami/kirigami-logo-universal.svg\" "
		"alt=\"Kirigami\" height=\"28\" /></td></tr>\n"
	);
	php_info_print_table_end();
	DISPLAY_INI_ENTRIES();
}

zend_module_entry jsonk_module_entry = {
	STANDARD_MODULE_HEADER_EX,
	NULL,
	jsonk_deps,
	"jsonk",
	jsonk_functions,
	PHP_MINIT(jsonk), PHP_MSHUTDOWN(jsonk),
	PHP_RINIT(jsonk), PHP_RSHUTDOWN(jsonk),
	PHP_MINFO(jsonk),
	PHP_JSONK_VERSION,
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_JSONK
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
#endif
ZEND_GET_MODULE(jsonk)
#endif
