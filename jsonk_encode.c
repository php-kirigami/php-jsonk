#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_jsonk.h"
#include "jsonk_error.h"
#include "jsonk_schema.h"
#include "jsonk_encode.h"
#include "ext/json/php_json.h"

#include <yyjson.h>
#include <math.h>

static bool build_val(yyjson_mut_doc *doc, zval *value, zend_long flags, zend_long depth_remaining, HashTable *visiting, yyjson_mut_val **out);

static bool build_key(yyjson_mut_doc *doc, zend_string *key, yyjson_mut_val **out)
{
	*out = yyjson_mut_strncpy(doc, ZSTR_VAL(key), ZSTR_LEN(key));
	return *out != NULL;
}

static bool build_array(yyjson_mut_doc *doc, zval *value, zend_long flags, zend_long depth_remaining, HashTable *visiting, yyjson_mut_val **out)
{
	HashTable *ht = Z_ARRVAL_P(value);
	bool is_list = !(flags & PHP_JSON_FORCE_OBJECT) && zend_array_is_list(ht);
	zend_string *key;
	zend_ulong idx;
	zval *item;

	if (is_list) {
		*out = yyjson_mut_arr(doc);
		if (!*out) return false;
		ZEND_HASH_FOREACH_VAL(ht, item) {
			yyjson_mut_val *item_val;
			if (!build_val(doc, item, flags, depth_remaining - 1, visiting, &item_val)) {
				return false;
			}
			yyjson_mut_arr_append(*out, item_val);
		} ZEND_HASH_FOREACH_END();
		return true;
	}

	*out = yyjson_mut_obj(doc);
	if (!*out) return false;

	ZEND_HASH_FOREACH_KEY_VAL(ht, idx, key, item) {
		yyjson_mut_val *key_val, *item_val;
		zend_string *tmp_key = NULL;
		zend_string *use_key = key;

		if (!use_key) {
			/* Numeric key in a non-list array: stringify it, matching
			 * json_encode()'s own behavior for mixed-key/non-sequential
			 * arrays serialized as a JSON object. */
			tmp_key = zend_long_to_str((zend_long) idx);
			use_key = tmp_key;
		}

		if (!build_key(doc, use_key, &key_val) || !build_val(doc, item, flags, depth_remaining - 1, visiting, &item_val)) {
			if (tmp_key) zend_string_release(tmp_key);
			return false;
		}
		yyjson_mut_obj_add(*out, key_val, item_val);
		if (tmp_key) zend_string_release(tmp_key);
	} ZEND_HASH_FOREACH_END();
	return true;
}

static bool build_object(yyjson_mut_doc *doc, zval *value, zend_long flags, zend_long depth_remaining, HashTable *visiting, yyjson_mut_val **out)
{
	zend_object *obj = Z_OBJ_P(value);
	HashTable *props;
	zend_string *key;
	zval *prop;

	if (zend_hash_index_exists(visiting, obj->handle)) {
		jsonk_set_error(JSONK_ERROR_RECURSION, "Recursion detected while encoding object");
		return false;
	}
	zend_hash_index_add_empty_element(visiting, obj->handle);

	/* Public properties only, same default ext/json uses for a plain
	 * object with no JsonSerializable -- which jsonk doesn't implement in
	 * v1, see CLAUDE.md. */
	props = Z_OBJPROP_P(value);
	*out = yyjson_mut_obj(doc);
	if (!*out) {
		zend_hash_index_del(visiting, obj->handle);
		return false;
	}

	ZEND_HASH_FOREACH_STR_KEY_VAL(props, key, prop) {
		yyjson_mut_val *key_val, *item_val;
		if (!key) continue;
		if (!build_key(doc, key, &key_val) || !build_val(doc, prop, flags, depth_remaining - 1, visiting, &item_val)) {
			zend_hash_index_del(visiting, obj->handle);
			return false;
		}
		yyjson_mut_obj_add(*out, key_val, item_val);
	} ZEND_HASH_FOREACH_END();

	zend_hash_index_del(visiting, obj->handle);
	return true;
}

static bool build_val(yyjson_mut_doc *doc, zval *value, zend_long flags, zend_long depth_remaining, HashTable *visiting, yyjson_mut_val **out)
{
	/* A zval pulled from an OBJECT's property table (Z_OBJPROP_P(), used
	 * for every declared/typed property) is commonly IS_INDIRECT --
	 * pointing at the real value inside the object's own storage rather
	 * than holding it directly. Confirmed as a real bug by an actual
	 * runtime test: encoding an object with a declared property produced
	 * JSONK_ERROR_UNSUPPORTED_TYPE for that property instead of its real
	 * value. Plain array values are never IS_INDIRECT, so this is a
	 * no-op for them. */
	if (Z_ISREF_P(value)) value = Z_REFVAL_P(value);
	if (Z_TYPE_P(value) == IS_INDIRECT) {
		value = Z_INDIRECT_P(value);
		if (Z_ISREF_P(value)) value = Z_REFVAL_P(value);
	}

	if (depth_remaining <= 0) {
		jsonk_set_error(JSONK_ERROR_DEPTH, "Maximum stack depth exceeded");
		return false;
	}

	switch (Z_TYPE_P(value)) {
		case IS_NULL:
			*out = yyjson_mut_null(doc);
			return true;
		case IS_TRUE:
			*out = yyjson_mut_bool(doc, 1);
			return true;
		case IS_FALSE:
			*out = yyjson_mut_bool(doc, 0);
			return true;
		case IS_LONG:
			*out = yyjson_mut_int(doc, (int64_t) Z_LVAL_P(value));
			return true;
		case IS_DOUBLE: {
			double d = Z_DVAL_P(value);
			if (zend_isnan(d) || zend_isinf(d)) {
				jsonk_set_error(JSONK_ERROR_UNSUPPORTED_TYPE, "Inf and NaN cannot be JSON encoded");
				return false;
			}
			/* PHP's own json_encode() collapses a whole-number float to
			 * an integer-looking JSON token (no decimal point) unless
			 * JSON_PRESERVE_ZERO_FRACTION is set. yyjson's writer has no
			 * equivalent flag -- a real/double-typed node always keeps
			 * its decimal point. Found by an actual round-trip test, not
			 * just a missing nice-to-have: without this, a whole float
			 * like 0.0 encoded as "0.0" (native: "0"), and decoding that
			 * back gave a PHP float where native gives an int -- a
			 * genuine fidelity break versus PHP's own default behavior,
			 * not merely a missing flag. Fixed by choosing an
			 * INTEGER-typed yyjson node for a value that's actually
			 * whole (and within int64_t's exact range), since yyjson's
			 * writer only ever adds a decimal point for a genuinely
			 * double-typed node -- no yyjson write-flag needed. */
			if (!(flags & PHP_JSON_PRESERVE_ZERO_FRACTION) &&
				d == (double) (int64_t) d && fabs(d) < 1e15
			) {
				*out = yyjson_mut_int(doc, (int64_t) d);
			} else {
				*out = yyjson_mut_real(doc, d);
			}
			return true;
		}
		case IS_STRING: {
			zend_string *str = Z_STR_P(value);
			if (flags & PHP_JSON_NUMERIC_CHECK) {
				zend_long lval;
				double dval;
				uint8_t type = is_numeric_string(ZSTR_VAL(str), ZSTR_LEN(str), &lval, &dval, 0);
				if (type == IS_LONG) {
					*out = yyjson_mut_int(doc, (int64_t) lval);
					return true;
				}
				if (type == IS_DOUBLE) {
					*out = yyjson_mut_real(doc, dval);
					return true;
				}
			}
			*out = yyjson_mut_strncpy(doc, ZSTR_VAL(str), ZSTR_LEN(str));
			return *out != NULL;
		}
		case IS_ARRAY:
			return build_array(doc, value, flags, depth_remaining, visiting, out);
		case IS_OBJECT:
			return build_object(doc, value, flags, depth_remaining, visiting, out);
		default:
			if (flags & PHP_JSON_PARTIAL_OUTPUT_ON_ERROR) {
				jsonk_set_error(JSONK_ERROR_UNSUPPORTED_TYPE, "Type is not supported, encoded as null");
				*out = yyjson_mut_null(doc);
				return true;
			}
			jsonk_set_error(JSONK_ERROR_UNSUPPORTED_TYPE, "Type is not supported");
			return false;
	}
}

zend_string *jsonk_encode_impl(zval *value, jsonk_schema_node *schema, zend_long flags, zend_long depth)
{
	yyjson_mut_doc *doc;
	yyjson_mut_val *root = NULL;
	yyjson_write_flag write_flags = YYJSON_WRITE_NOFLAG;
	char *json;
	size_t json_len;
	zend_string *result;
	HashTable visiting;
	bool ok;

	if (schema) {
		smart_str path = {0};
		bool valid = jsonk_schema_validate_zval(schema, value, &path, depth, NULL, NULL);
		smart_str_free(&path);
		if (!valid) {
			return NULL;
		}
	}

	doc = yyjson_mut_doc_new(NULL);
	if (!doc) {
		jsonk_set_error(JSONK_ERROR_UNSUPPORTED_TYPE, "Failed to allocate encoder document");
		return NULL;
	}

	zend_hash_init(&visiting, 8, NULL, NULL, 0);
	ok = build_val(doc, value, flags, depth, &visiting, &root);
	zend_hash_destroy(&visiting);

	if (!ok) {
		yyjson_mut_doc_free(doc);
		return NULL;
	}

	yyjson_mut_doc_set_root(doc, root);

	/* PHP escapes slashes and non-ASCII by default; yyjson's defaults are
	 * the opposite (see CLAUDE.md), so the JSON_UNESCAPED_* flags being
	 * ABSENT is what turns escaping ON here. */
	if (!(flags & PHP_JSON_UNESCAPED_SLASHES))  write_flags |= YYJSON_WRITE_ESCAPE_SLASHES;
	if (!(flags & PHP_JSON_UNESCAPED_UNICODE))  write_flags |= YYJSON_WRITE_ESCAPE_UNICODE;
	if (flags & PHP_JSON_PRETTY_PRINT)          write_flags |= YYJSON_WRITE_PRETTY;

	json = yyjson_mut_write_opts(doc, write_flags, NULL, &json_len, NULL);
	yyjson_mut_doc_free(doc);

	if (!json) {
		jsonk_set_error(JSONK_ERROR_UNSUPPORTED_TYPE, "yyjson failed to write the document");
		return NULL;
	}

	result = zend_string_init(json, json_len, 0);
	free(json);
	return result;
}
