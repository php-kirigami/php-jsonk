/* simdjson-based decode. This is the one C++ translation unit in the
 * extension (see docs/DECISIONS.md for why): it includes both php.h and
 * simdjson.h directly and does the whole DOM-walk-to-zval conversion here,
 * rather than exposing a generic C accessor API over simdjson's DOM types
 * across an extern "C" boundary (rejected -- see docs/DECISIONS.md's "simdjson
 * bridge" decision for the reasoning). jsonk_decode_impl() is the only
 * symbol this file exposes to the rest of the (C) extension. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_jsonk.h"
#include "jsonk_error.h"
#include "jsonk_decode.h"

extern "C" {
#include "ext/json/php_json.h"
}

#include <simdjson.h>
#include <cstring>
#include <cstdio>
#include <string>

using namespace simdjson;

static bool build_zval(dom::element el, zend_long flags, zend_long depth_remaining, zval *out)
{
	if (depth_remaining <= 0) {
		jsonk_set_error(JSONK_ERROR_DEPTH, "Maximum stack depth exceeded");
		ZVAL_NULL(out);
		return false;
	}

	switch (el.type()) {
		case dom::element_type::NULL_VALUE:
			ZVAL_NULL(out);
			return true;

		case dom::element_type::BOOL: {
			bool b = false;
			el.get_bool().get(b);
			ZVAL_BOOL(out, b);
			return true;
		}

		case dom::element_type::INT64: {
			int64_t v = 0;
			el.get_int64().get(v);
			ZVAL_LONG(out, (zend_long) v);
			return true;
		}

		case dom::element_type::UINT64: {
			uint64_t v = 0;
			el.get_uint64().get(v);
			if (v <= (uint64_t) ZEND_LONG_MAX) {
				ZVAL_LONG(out, (zend_long) v);
			} else if (flags & PHP_JSON_BIGINT_AS_STRING) {
				char buf[32];
				int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long) v);
				ZVAL_STRINGL(out, buf, n);
			} else {
				ZVAL_DOUBLE(out, (double) v);
			}
			return true;
		}

		case dom::element_type::BIGINT: {
			/* A number too large even for uint64_t -- simdjson keeps it as
			 * a raw digit string. */
			std::string_view sv;
			el.get_bigint().get(sv);
			if (flags & PHP_JSON_BIGINT_AS_STRING) {
				ZVAL_STRINGL(out, sv.data(), (int) sv.size());
			} else {
				ZVAL_DOUBLE(out, strtod(std::string(sv).c_str(), nullptr));
			}
			return true;
		}

		case dom::element_type::DOUBLE: {
			double d = 0.0;
			el.get_double().get(d);
			ZVAL_DOUBLE(out, d);
			return true;
		}

		case dom::element_type::STRING: {
			std::string_view sv;
			el.get_string().get(sv);
			ZVAL_STRINGL(out, sv.data(), (int) sv.size());
			return true;
		}

		case dom::element_type::ARRAY: {
			dom::array arr;
			el.get_array().get(arr);
			array_init(out);
			for (dom::element item : arr) {
				zval item_zv;
				if (!build_zval(item, flags, depth_remaining - 1, &item_zv)) {
					zval_ptr_dtor(out);
					ZVAL_NULL(out);
					return false;
				}
				add_next_index_zval(out, &item_zv);
			}
			return true;
		}

		case dom::element_type::OBJECT: {
			dom::object obj;
			el.get_object().get(obj);
			bool as_assoc = (flags & PHP_JSON_OBJECT_AS_ARRAY) != 0;

			if (as_assoc) {
				array_init(out);
			} else {
				object_init(out);
			}

			for (dom::key_value_pair field : obj) {
				zval val_zv;
				if (!build_zval(field.value, flags, depth_remaining - 1, &val_zv)) {
					zval_ptr_dtor(out);
					ZVAL_NULL(out);
					return false;
				}
				if (as_assoc) {
					add_assoc_zval_ex(out, field.key.data(), field.key.size(), &val_zv);
				} else {
					zend_string *zkey = zend_string_init(field.key.data(), field.key.size(), 0);
					zend_update_property_ex(Z_OBJCE_P(out), Z_OBJ_P(out), zkey, &val_zv);
					zval_ptr_dtor(&val_zv);
					zend_string_release(zkey);
				}
			}
			return true;
		}

		default:
			jsonk_set_error(JSONK_ERROR_SYNTAX, "Unsupported JSON value encountered while decoding");
			ZVAL_NULL(out);
			return false;
	}
}

extern "C" int jsonk_decode_impl(const char *json, size_t json_len, zend_long flags, zend_long depth, zval *return_value)
{
	dom::parser parser;
	dom::element root;
	error_code error;

	error = parser.parse(json, json_len).get(root);
	if (error) {
		if (error == DEPTH_ERROR) {
			jsonk_set_error(JSONK_ERROR_DEPTH, "%s", error_message(error));
		} else if (error == UTF8_ERROR) {
			jsonk_set_error(JSONK_ERROR_UTF8, "%s", error_message(error));
		} else {
			jsonk_set_error(JSONK_ERROR_SYNTAX, "%s", error_message(error));
		}
		ZVAL_NULL(return_value);
		return 0;
	}

	return build_zval(root, flags, depth, return_value) ? 1 : 0;
}
