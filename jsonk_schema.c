#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_jsonk.h"
#include "jsonk_error.h"
#include "jsonk_schema.h"
#include "jsonk_regex.h"
#include "jsonk_format.h"
#include "jsonk_fetch.h"
#include <yyjson.h>
#include <math.h>
#include <string.h>

/* ========================================================================
 * Compilation context -- threaded through every compile_node()/
 * fill_node_from_schema() call.
 *
 * `root_doc` is kept alive for the WHOLE compile (unlike v1, which freed
 * it right after the top-level compile_node() call) because "$ref"
 * resolution (see compile_ref()) can walk back into it at any point
 * during compilation, via yyjson's own built-in JSON Pointer support
 * (yyjson_doc_ptr_getn() -- RFC 6901).
 *
 * `current_doc` is which document a bare "#/json/pointer" fragment
 * currently resolves against -- `root_doc` at the top level, but
 * temporarily swapped to an externally-fetched document's own
 * `yyjson_doc` while compiling ITS content (see compile_ref()), so a
 * fragment "$ref" found inside a fetched external schema resolves within
 * that document, not back against the original top-level one. Restored
 * once that content is done compiling.
 *
 * `current_base_uri` is the CURRENT document's own base URI for
 * resolving a RELATIVE "$ref" (e.g. "../plugin-foo/options.schema.json")
 * -- NULL if unknown (no relative "$ref" can be resolved in that case,
 * a compile error rather than a guess). Per the JSON Schema spec, a
 * document's base URI is its own top-level "$id" if it declares one, or
 * (only relevant for an externally-fetched document, never the top-level
 * schema, which has no URL of its own) the URL it was actually fetched
 * from. Swapped alongside `current_doc` for the same reason.
 *
 * `ref_cache` maps a resolution-qualified "$ref" string (the raw "$ref"
 * text, prefixed with which document it was resolved against -- see
 * build_ref_cache_key()) to the jsonk_schema_node* it resolves to. This is
 * what makes a *recursive* schema (e.g. a self-referential tree/linked-
 * list shape) compile at all instead of infinite-looping: compile_ref()
 * registers a target's node BEFORE recursively filling it in, so an inner
 * "$ref" back to the same target resolves to that same (still-being-
 * filled) pointer. The document-qualified key also stops two different
 * documents that happen to share the same bare fragment text (e.g. both
 * defining "#/$defs/foo") from colliding.
 *
 * `arena`/`arena_count` track EVERY jsonk_schema_node ever allocated
 * during this compile. This exists because "$ref" makes the compiled
 * schema a DAG (possibly with cycles) rather than a strict tree -- a
 * naive "each node recursively frees its children" jsonk_schema_free()
 * would double-free a shared/cached ref target, or infinite-loop on a
 * cyclic one. Freeing every arena entry exactly once, without ever
 * following a child pointer to decide what to free, sidesteps both
 * problems. See jsonk_schema_free().
 * ======================================================================== */
typedef struct {
	yyjson_doc *root_doc;
	yyjson_doc *current_doc;
	zend_string *current_base_uri; /* NULL = unknown, see doc comment above */
	HashTable ref_cache;
	jsonk_schema_node **arena;
	uint32_t arena_count;
	uint32_t arena_capacity;
} jsonk_compile_ctx;

static jsonk_schema_node *compile_node(yyjson_val *schema, jsonk_compile_ctx *ctx);
static bool fill_node_from_schema(jsonk_schema_node *node, yyjson_val *schema, jsonk_compile_ctx *ctx);
static bool compile_schema_array(yyjson_val *arr, jsonk_compile_ctx *ctx, jsonk_schema_node ***out, uint32_t *out_count);
static void jsonk_schema_node_free_shallow(jsonk_schema_node *node);

static jsonk_schema_node *jsonk_schema_node_new(jsonk_compile_ctx *ctx)
{
	jsonk_schema_node *node = ecalloc(1, sizeof(jsonk_schema_node));

	if (ctx->arena_count == ctx->arena_capacity) {
		ctx->arena_capacity = ctx->arena_capacity ? ctx->arena_capacity * 2 : 16;
		ctx->arena = erealloc(ctx->arena, ctx->arena_capacity * sizeof(jsonk_schema_node *));
	}
	ctx->arena[ctx->arena_count++] = node;

	return node;
}

static bool parse_type_name(const char *name, size_t len, uint32_t *mask)
{
	if (len == 4 && memcmp(name, "null", 4) == 0)    { *mask |= JSONK_TYPE_NULL;   return true; }
	if (len == 7 && memcmp(name, "boolean", 7) == 0) { *mask |= JSONK_TYPE_BOOL;   return true; }
	if (len == 6 && memcmp(name, "object", 6) == 0)  { *mask |= JSONK_TYPE_OBJECT; return true; }
	if (len == 5 && memcmp(name, "array", 5) == 0)   { *mask |= JSONK_TYPE_ARRAY;  return true; }
	if (len == 6 && memcmp(name, "number", 6) == 0)  { *mask |= JSONK_TYPE_NUMBER; return true; }
	if (len == 6 && memcmp(name, "string", 6) == 0)  { *mask |= JSONK_TYPE_STRING; return true; }
	if (len == 7 && memcmp(name, "integer", 7) == 0) { *mask |= JSONK_TYPE_INT;    return true; }
	return false;
}

static bool compile_type(yyjson_val *type_val, uint32_t *out_mask)
{
	*out_mask = JSONK_TYPE_ANY;

	if (yyjson_is_str(type_val)) {
		if (!parse_type_name(yyjson_get_str(type_val), yyjson_get_len(type_val), out_mask)) {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "Unknown schema \"type\" value");
			return false;
		}
		return true;
	}

	if (yyjson_is_arr(type_val)) {
		yyjson_arr_iter iter;
		yyjson_val *item;
		yyjson_arr_iter_init(type_val, &iter);
		while ((item = yyjson_arr_iter_next(&iter))) {
			if (!yyjson_is_str(item) || !parse_type_name(yyjson_get_str(item), yyjson_get_len(item), out_mask)) {
				jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "Invalid entry in schema \"type\" array");
				return false;
			}
		}
		return true;
	}

	jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "Schema \"type\" must be a string or an array of strings");
	return false;
}

/* Materializes an arbitrary yyjson literal (used for "enum"/"const") into a
 * zval tree. Independent of the compiled schema tree afterward -- copies
 * everything, no dependency on the source yyjson_doc surviving. JSON objects
 * become stdClass, like jsonk_decode()'s default, so an empty {} stays
 * distinguishable from []. */
static void yyjson_val_to_zval(yyjson_val *v, zval *out)
{
	switch (yyjson_get_type(v)) {
		case YYJSON_TYPE_NULL:
			ZVAL_NULL(out);
			break;
		case YYJSON_TYPE_BOOL:
			ZVAL_BOOL(out, yyjson_get_bool(v));
			break;
		case YYJSON_TYPE_NUM:
			if (yyjson_is_int(v)) {
				if (yyjson_get_subtype(v) == YYJSON_SUBTYPE_UINT) {
					uint64_t u = yyjson_get_uint(v);
					if (u <= (uint64_t) ZEND_LONG_MAX) {
						ZVAL_LONG(out, (zend_long) u);
					} else {
						ZVAL_DOUBLE(out, (double) u);
					}
				} else {
					ZVAL_LONG(out, (zend_long) yyjson_get_sint(v));
				}
			} else {
				ZVAL_DOUBLE(out, yyjson_get_real(v));
			}
			break;
		case YYJSON_TYPE_STR:
			ZVAL_STRINGL(out, yyjson_get_str(v), yyjson_get_len(v));
			break;
		case YYJSON_TYPE_ARR: {
			yyjson_arr_iter iter;
			yyjson_val *item;
			array_init(out);
			yyjson_arr_iter_init(v, &iter);
			while ((item = yyjson_arr_iter_next(&iter))) {
				zval item_zv;
				yyjson_val_to_zval(item, &item_zv);
				add_next_index_zval(out, &item_zv);
			}
			break;
		}
		case YYJSON_TYPE_OBJ: {
			yyjson_obj_iter iter;
			yyjson_val *key, *val;
			HashTable *props;
			object_init(out);
			props = Z_OBJPROP_P(out);
			yyjson_obj_iter_init(v, &iter);
			while ((key = yyjson_obj_iter_next(&iter))) {
				zval val_zv;
				val = yyjson_obj_iter_get_val(key);
				yyjson_val_to_zval(val, &val_zv);
				/* Property names stay string keys, even numeric ones. */
				zend_hash_str_update(props, yyjson_get_str(key), yyjson_get_len(key), &val_zv);
			}
			break;
		}
		default:
			ZVAL_NULL(out);
			break;
	}
}

static bool compile_schema_array(yyjson_val *arr, jsonk_compile_ctx *ctx, jsonk_schema_node ***out, uint32_t *out_count)
{
	yyjson_arr_iter iter;
	yyjson_val *item;
	uint32_t i = 0;

	if (!yyjson_is_arr(arr) || yyjson_arr_size(arr) == 0) {
		jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"allOf\"/\"anyOf\"/\"oneOf\"/\"prefixItems\" must be a non-empty array of schemas");
		return false;
	}

	*out_count = (uint32_t) yyjson_arr_size(arr);
	*out = emalloc(*out_count * sizeof(jsonk_schema_node *));

	yyjson_arr_iter_init(arr, &iter);
	while ((item = yyjson_arr_iter_next(&iter))) {
		(*out)[i] = compile_node(item, ctx);
		if (!(*out)[i]) {
			*out_count = i; /* exclude the failed slot -- the arena still owns and frees every node allocated so far, see jsonk_schema_compile() */
			return false;
		}
		i++;
	}
	return true;
}

/* Builds a ref_cache key that's qualified by WHICH document a bare
 * fragment is being resolved against (`ctx->current_doc`'s own pointer
 * identity, stable for the lifetime of the compile) -- so the same
 * fragment text ("#/$defs/foo") found in two different documents (the
 * top-level schema and a fetched external one) never collides. An
 * external URL is already globally unique on its own, but gets the same
 * treatment for simplicity (harmless: at worst it means the same external
 * URL referenced from two different documents is fetched from cache and
 * compiled twice instead of once -- see docs/DECISIONS.md). Truncates a
 * pathologically long "$ref" rather than overflow `buf`; a collision from
 * that is only possible between two such long refs differing solely in
 * their truncated tail, an unrealistic edge case. */
static size_t build_ref_cache_key(jsonk_compile_ctx *ctx, const char *ref_str, size_t ref_len, char *buf, size_t buf_size)
{
	int n = snprintf(buf, buf_size, "%p|", (void *) ctx->current_doc);
	size_t prefix_len = (n > 0 && (size_t) n < buf_size) ? (size_t) n : 0;
	size_t copy_len = ref_len;

	if (prefix_len + copy_len >= buf_size) {
		copy_len = buf_size - prefix_len - 1;
	}
	memcpy(buf + prefix_len, ref_str, copy_len);
	return prefix_len + copy_len;
}

/* Resolves `ref` (a RELATIVE reference, e.g. "../plugin-foo/options.json"
 * or "sibling.json") against `base` (an absolute http(s) URL, e.g. a
 * document's own "$id"), producing a new absolute URL. Implements enough
 * of RFC 3986 SS5.3 (merge + remove-dot-segments) for real-world
 * multi-file schema layouts -- sibling files and "../" parent
 * references -- not a full RFC 3986 parser (no query-string handling,
 * scheme-relative "//host/path" references, or "?"/authority edge cases
 * beyond a plain relative path). Returns NULL if `base` isn't a
 * well-formed "scheme://..." URL (shouldn't happen -- every caller
 * already validated `base` itself came from a successfully-fetched
 * http(s) URL or a same-shaped "$id"). */
static zend_string *resolve_relative_uri(const char *base, size_t base_len, const char *ref, size_t ref_len)
{
	const char *scheme_end, *path_start, *authority_end, *last_slash, *p, *end;
	smart_str merged = {0};
	smart_str result = {0};
	size_t prefix_len;

	scheme_end = memchr(base, ':', base_len);
	if (!scheme_end || (size_t) ((base + base_len) - scheme_end) < 3 || scheme_end[1] != '/' || scheme_end[2] != '/') {
		return NULL;
	}
	path_start = scheme_end + 3;
	authority_end = memchr(path_start, '/', (base + base_len) - path_start);
	if (!authority_end) authority_end = base + base_len;
	prefix_len = (size_t) (authority_end - base);

	if (ref_len > 0 && ref[0] == '/') {
		/* absolute-path reference: keep scheme+authority, replace the path entirely */
		smart_str_appendl(&merged, base, prefix_len);
		smart_str_appendl(&merged, ref, ref_len);
	} else {
		/* relative-path reference: merge with the base's own path, up to its last '/' */
		last_slash = NULL;
		for (p = authority_end; p < base + base_len; p++) {
			if (*p == '/') last_slash = p;
		}
		if (last_slash) {
			smart_str_appendl(&merged, base, (size_t) ((last_slash + 1) - base));
		} else {
			smart_str_appendl(&merged, base, prefix_len);
			smart_str_appendc(&merged, '/');
		}
		smart_str_appendl(&merged, ref, ref_len);
	}

	/* remove-dot-segments (RFC 3986 SS5.2.4): scheme+authority is copied
	 * through untouched into `result` first, then the path is rebuilt
	 * segment by segment, dropping "." and resolving ".." against
	 * whatever's already been written to `result`. */
	smart_str_appendl(&result, ZSTR_VAL(merged.s), prefix_len);

	p = ZSTR_VAL(merged.s) + prefix_len;
	end = ZSTR_VAL(merged.s) + ZSTR_LEN(merged.s);
	while (p < end) {
		if (p[0] == '.' && (p + 1 == end || p[1] == '/')) {
			p += (p + 1 == end) ? 1 : 2;
		} else if (p[0] == '.' && p[1] == '.' && (p + 2 == end || p[2] == '/')) {
			p += (p + 2 == end) ? 2 : 3;
			if (result.s) {
				size_t rlen = ZSTR_LEN(result.s);
				if (rlen > prefix_len && ZSTR_VAL(result.s)[rlen - 1] == '/') rlen--;
				while (rlen > prefix_len && ZSTR_VAL(result.s)[rlen - 1] != '/') rlen--;
				ZSTR_LEN(result.s) = rlen;
			}
		} else {
			const char *seg_end = memchr(p, '/', end - p);
			size_t seg_len = seg_end ? (size_t) (seg_end - p) + 1 : (size_t) (end - p);
			smart_str_appendl(&result, p, seg_len);
			p += seg_len;
		}
	}

	smart_str_free(&merged);
	smart_str_0(&result);
	return result.s;
}

/* Resolves a "$ref": a same-document fragment ("#" or "#/json/pointer"),
 * an absolute http(s) URL (optionally with its own fragment), or now a
 * RELATIVE reference (e.g. "../plugin-foo/options.json") resolved against
 * `ctx->current_base_uri` -- fetched via jsonk_fetch_url() -- active by
 * default, no opt-in flag (see docs/DECISIONS.md's SSRF discussion). See
 * jsonk_schema.h's struct doc comment for why sibling keywords alongside
 * "$ref" are ignored (the returned node IS the target directly, not a
 * wrapper).
 *
 * While compiling content that came from an externally-fetched document,
 * `ctx->current_doc`/`current_base_uri` are temporarily swapped to that
 * document's own `yyjson_doc` and base URI (its own "$id" if it declares
 * one, else the URL it was fetched from), so a fragment or relative
 * "$ref" found INSIDE it resolves against that document, not the original
 * top-level one -- restored before returning either way. A relative
 * "$ref" with no known base (no enclosing "$id", and -- for the
 * top-level schema specifically -- none possible, since it's just a raw
 * string with no URL of its own) is a clear compile error, not a guess. */
static jsonk_schema_node *compile_ref(yyjson_val *ref_val, jsonk_compile_ctx *ctx)
{
	const char *ref_str;
	size_t ref_len;
	jsonk_schema_node *target;
	yyjson_val *target_val;
	yyjson_doc *ext_doc = NULL;
	zend_string *new_base = NULL; /* the fetched document's own base URI -- always set together with ext_doc, freed together too */
	char cache_key[2048];
	size_t cache_key_len;

	if (!yyjson_is_str(ref_val)) {
		jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"$ref\" must be a string");
		return NULL;
	}
	ref_str = yyjson_get_str(ref_val);
	ref_len = yyjson_get_len(ref_val);

	cache_key_len = build_ref_cache_key(ctx, ref_str, ref_len, cache_key, sizeof(cache_key));
	target = zend_hash_str_find_ptr(&ctx->ref_cache, cache_key, cache_key_len);
	if (target) {
		return target;
	}

	if (ref_len > 0 && ref_str[0] == '#') {
		target_val = yyjson_doc_ptr_getn(ctx->current_doc, ref_str + 1, ref_len - 1);
		if (!target_val) {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"%.*s\" does not resolve to anything in this schema document", (int) ref_len, ref_str);
			return NULL;
		}
	} else {
		const char *hash = memchr(ref_str, '#', ref_len);
		size_t path_len = hash ? (size_t) (hash - ref_str) : ref_len;
		const char *fetch_url;
		size_t fetch_url_len;
		zend_string *resolved_url = NULL;
		zend_string *body;
		yyjson_val *root_val, *id_val;

		if ((path_len > 7 && memcmp(ref_str, "http://", 7) == 0) || (path_len > 8 && memcmp(ref_str, "https://", 8) == 0)) {
			fetch_url = ref_str;
			fetch_url_len = path_len;
		} else if (ctx->current_base_uri) {
			/* A RELATIVE reference (e.g. "../plugin-foo/options.json"),
			 * resolved against the CURRENT document's own base URI --
			 * its "$id" if it declared one, or the URL it was itself
			 * fetched from (see below) -- not the original top-level
			 * schema's base, if this "$ref" is nested inside an
			 * externally-fetched document. */
			resolved_url = resolve_relative_uri(ZSTR_VAL(ctx->current_base_uri), ZSTR_LEN(ctx->current_base_uri), ref_str, path_len);
			if (!resolved_url) {
				jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "Could not resolve relative \"$ref\" \"%.*s\" against base URI \"%s\"", (int) ref_len, ref_str, ZSTR_VAL(ctx->current_base_uri));
				return NULL;
			}
			fetch_url = ZSTR_VAL(resolved_url);
			fetch_url_len = ZSTR_LEN(resolved_url);
		} else {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID,
				"\"$ref\": \"%.*s\" is relative, but its enclosing document has no \"$id\" to resolve it against "
				"(and, for the top-level schema, none was otherwise known)",
				(int) ref_len, ref_str);
			return NULL;
		}

		body = jsonk_fetch_url(fetch_url, fetch_url_len);
		if (!body) {
			if (resolved_url) zend_string_release(resolved_url);
			return NULL; /* jsonk_fetch_url() already called jsonk_set_error() */
		}

		ext_doc = yyjson_read_opts(ZSTR_VAL(body), ZSTR_LEN(body), YYJSON_READ_NOFLAG, NULL, NULL);
		zend_string_release(body);
		if (!ext_doc) {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "External \"$ref\" target is not valid JSON: %.*s", (int) fetch_url_len, fetch_url);
			if (resolved_url) zend_string_release(resolved_url);
			return NULL;
		}

		/* This fetched document's own base URI for resolving any FURTHER
		 * relative "$ref" found inside it: its own "$id" if it declares
		 * one (per spec, "$id" always wins over the fetch URL), else the
		 * URL it was actually fetched from. */
		root_val = yyjson_doc_get_root(ext_doc);
		id_val = yyjson_is_obj(root_val) ? yyjson_obj_get(root_val, "$id") : NULL;
		if (id_val && yyjson_is_str(id_val)) {
			new_base = zend_string_init(yyjson_get_str(id_val), yyjson_get_len(id_val), 0);
		} else {
			new_base = zend_string_init(fetch_url, fetch_url_len, 0);
		}
		if (resolved_url) zend_string_release(resolved_url);

		if (hash && ref_len > path_len + 1) {
			target_val = yyjson_doc_ptr_getn(ext_doc, hash + 1, ref_len - path_len - 1);
		} else {
			target_val = root_val;
		}
		if (!target_val) {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "External \"$ref\" fragment not found: %.*s", (int) ref_len, ref_str);
			zend_string_release(new_base);
			yyjson_doc_free(ext_doc);
			return NULL;
		}
	}

	if (yyjson_is_bool(target_val)) {
		target = jsonk_schema_node_new(ctx);
		target->is_bool_schema = true;
		target->bool_schema_value = yyjson_get_bool(target_val);
		zend_hash_str_update_ptr(&ctx->ref_cache, cache_key, cache_key_len, target);
	} else if (yyjson_is_obj(target_val)) {
		/* Registered BEFORE recursively filling it in -- this is what
		 * makes a self-referential schema (directly, or through another
		 * "$ref") resolve to this same pointer instead of recursing
		 * forever during compilation, e.g. a tree-shaped schema like
		 * {"$defs": {"node": {"properties": {"children": {"type": "array",
		 * "items": {"$ref": "#/$defs/node"}}}}}}. */
		yyjson_doc *saved_doc = ctx->current_doc;
		zend_string *saved_base = ctx->current_base_uri;
		bool ok;

		target = jsonk_schema_node_new(ctx);
		zend_hash_str_update_ptr(&ctx->ref_cache, cache_key, cache_key_len, target);

		if (ext_doc) {
			ctx->current_doc = ext_doc;
			ctx->current_base_uri = new_base;
		}
		ok = fill_node_from_schema(target, target_val, ctx);
		ctx->current_doc = saved_doc;
		ctx->current_base_uri = saved_base;

		if (!ok) {
			/* `target` is already in the arena, so it's still freed by
			 * jsonk_schema_compile()'s failure path. */
			if (ext_doc) { yyjson_doc_free(ext_doc); zend_string_release(new_base); }
			return NULL;
		}
	} else {
		jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"%.*s\" does not resolve to a schema (object or boolean)", (int) ref_len, ref_str);
		if (ext_doc) { yyjson_doc_free(ext_doc); zend_string_release(new_base); }
		return NULL;
	}

	if (ext_doc) { yyjson_doc_free(ext_doc); zend_string_release(new_base); }
	return target;
}

static bool fill_node_from_schema(jsonk_schema_node *node, yyjson_val *schema, jsonk_compile_ctx *ctx)
{
	yyjson_val *v;

	if ((v = yyjson_obj_get(schema, "type")) != NULL && !compile_type(v, &node->type_mask)) {
		return false;
	}

	if ((v = yyjson_obj_get(schema, "enum")) != NULL) {
		if (!yyjson_is_arr(v)) {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"enum\" must be an array");
			return false;
		}
		node->has_enum = true;
		node->enum_count = (uint32_t) yyjson_arr_size(v);
		node->enum_values = ecalloc(node->enum_count ? node->enum_count : 1, sizeof(zval));
		{
			yyjson_arr_iter iter;
			yyjson_val *item;
			uint32_t i = 0;
			yyjson_arr_iter_init(v, &iter);
			while ((item = yyjson_arr_iter_next(&iter))) {
				yyjson_val_to_zval(item, &node->enum_values[i++]);
			}
		}
	}

	if ((v = yyjson_obj_get(schema, "const")) != NULL) {
		node->has_const = true;
		yyjson_val_to_zval(v, &node->const_value);
	}

	/* string */
	if ((v = yyjson_obj_get(schema, "minLength")) != NULL && yyjson_is_int(v)) {
		node->has_min_length = true;
		node->min_length = (uint32_t) yyjson_get_uint(v);
	}
	if ((v = yyjson_obj_get(schema, "maxLength")) != NULL && yyjson_is_int(v)) {
		node->has_max_length = true;
		node->max_length = (uint32_t) yyjson_get_uint(v);
	}
	if ((v = yyjson_obj_get(schema, "pattern")) != NULL) {
		if (!yyjson_is_str(v)) {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"pattern\" must be a string");
			return false;
		}
		node->pattern = zend_string_init(yyjson_get_str(v), yyjson_get_len(v), 0);
		if (!jsonk_regex_is_valid(ZSTR_VAL(node->pattern), ZSTR_LEN(node->pattern))) {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"pattern\" is not a valid regular expression");
			return false;
		}
	}
	if ((v = yyjson_obj_get(schema, "format")) != NULL && yyjson_is_str(v)) {
		node->format = zend_string_init(yyjson_get_str(v), yyjson_get_len(v), 0);
	}

	/* number / integer */
	if ((v = yyjson_obj_get(schema, "minimum")) != NULL && yyjson_is_num(v)) {
		node->has_minimum = true;
		node->minimum = yyjson_get_num(v);
	}
	if ((v = yyjson_obj_get(schema, "maximum")) != NULL && yyjson_is_num(v)) {
		node->has_maximum = true;
		node->maximum = yyjson_get_num(v);
	}
	if ((v = yyjson_obj_get(schema, "exclusiveMinimum")) != NULL && yyjson_is_num(v)) {
		node->has_minimum = true;
		node->minimum = yyjson_get_num(v);
		node->exclusive_minimum = true;
	}
	if ((v = yyjson_obj_get(schema, "exclusiveMaximum")) != NULL && yyjson_is_num(v)) {
		node->has_maximum = true;
		node->maximum = yyjson_get_num(v);
		node->exclusive_maximum = true;
	}
	if ((v = yyjson_obj_get(schema, "multipleOf")) != NULL && yyjson_is_num(v)) {
		node->has_multiple_of = true;
		node->multiple_of = yyjson_get_num(v);
	}

	/* array */
	if ((v = yyjson_obj_get(schema, "prefixItems")) != NULL) {
		if (!compile_schema_array(v, ctx, &node->prefix_items, &node->prefix_items_count)) {
			return false;
		}
	}
	if ((v = yyjson_obj_get(schema, "items")) != NULL) {
		node->items = compile_node(v, ctx);
		if (!node->items) return false;
	} else if ((v = yyjson_obj_get(schema, "additionalItems")) != NULL) {
		/* draft-07's name for "items beyond what prefixItems (there,
		 * tuple-form 'items') covers" -- accepted as a fallback so older
		 * real-world schemas still work when the modern "items" keyword
		 * is absent. */
		node->items = compile_node(v, ctx);
		if (!node->items) return false;
	}
	if ((v = yyjson_obj_get(schema, "minItems")) != NULL && yyjson_is_int(v)) {
		node->has_min_items = true;
		node->min_items = (uint32_t) yyjson_get_uint(v);
	}
	if ((v = yyjson_obj_get(schema, "maxItems")) != NULL && yyjson_is_int(v)) {
		node->has_max_items = true;
		node->max_items = (uint32_t) yyjson_get_uint(v);
	}
	if ((v = yyjson_obj_get(schema, "uniqueItems")) != NULL && yyjson_is_bool(v)) {
		node->unique_items = yyjson_get_bool(v);
	}
	if ((v = yyjson_obj_get(schema, "contains")) != NULL) {
		node->contains = compile_node(v, ctx);
		if (!node->contains) return false;
		node->has_min_contains = true;
		node->min_contains = 1; /* spec default when "contains" is present */
		if ((v = yyjson_obj_get(schema, "minContains")) != NULL && yyjson_is_int(v)) {
			node->min_contains = (uint32_t) yyjson_get_uint(v);
		}
		if ((v = yyjson_obj_get(schema, "maxContains")) != NULL && yyjson_is_int(v)) {
			node->has_max_contains = true;
			node->max_contains = (uint32_t) yyjson_get_uint(v);
		}
	}
	if ((v = yyjson_obj_get(schema, "unevaluatedItems")) != NULL) {
		node->unevaluated_items = compile_node(v, ctx);
		if (!node->unevaluated_items) return false;
	}

	/* object */
	if ((v = yyjson_obj_get(schema, "properties")) != NULL) {
		if (!yyjson_is_obj(v)) {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"properties\" must be an object");
			return false;
		}
		node->properties_count = (uint32_t) yyjson_obj_size(v);
		node->properties = emalloc((node->properties_count ? node->properties_count : 1) * sizeof(jsonk_schema_property));
		{
			yyjson_obj_iter iter;
			yyjson_val *key, *val;
			uint32_t i = 0;
			yyjson_obj_iter_init(v, &iter);
			while ((key = yyjson_obj_iter_next(&iter))) {
				val = yyjson_obj_iter_get_val(key);
				node->properties[i].name = zend_string_init(yyjson_get_str(key), yyjson_get_len(key), 0);
				node->properties[i].schema = compile_node(val, ctx);
				if (!node->properties[i].schema) {
					node->properties_count = i; /* leaks this one entry's .name on this compile-failure-only path */
					return false;
				}
				i++;
			}
		}
	}

	if ((v = yyjson_obj_get(schema, "required")) != NULL) {
		if (!yyjson_is_arr(v)) {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"required\" must be an array of strings");
			return false;
		}
		node->required_count = (uint32_t) yyjson_arr_size(v);
		node->required = emalloc((node->required_count ? node->required_count : 1) * sizeof(zend_string *));
		{
			yyjson_arr_iter iter;
			yyjson_val *item;
			uint32_t i = 0;
			yyjson_arr_iter_init(v, &iter);
			while ((item = yyjson_arr_iter_next(&iter))) {
				if (!yyjson_is_str(item)) {
					node->required_count = i;
					jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"required\" must be an array of strings");
					return false;
				}
				node->required[i++] = zend_string_init(yyjson_get_str(item), yyjson_get_len(item), 0);
			}
		}
	}

	if ((v = yyjson_obj_get(schema, "additionalProperties")) != NULL) {
		node->additional_properties = compile_node(v, ctx);
		if (!node->additional_properties) return false;
	}

	if ((v = yyjson_obj_get(schema, "minProperties")) != NULL && yyjson_is_int(v)) {
		node->has_min_properties = true;
		node->min_properties = (uint32_t) yyjson_get_uint(v);
	}
	if ((v = yyjson_obj_get(schema, "maxProperties")) != NULL && yyjson_is_int(v)) {
		node->has_max_properties = true;
		node->max_properties = (uint32_t) yyjson_get_uint(v);
	}

	if ((v = yyjson_obj_get(schema, "patternProperties")) != NULL) {
		if (!yyjson_is_obj(v)) {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"patternProperties\" must be an object");
			return false;
		}
		node->pattern_properties_count = (uint32_t) yyjson_obj_size(v);
		node->pattern_properties = emalloc((node->pattern_properties_count ? node->pattern_properties_count : 1) * sizeof(jsonk_schema_property));
		{
			yyjson_obj_iter iter;
			yyjson_val *key, *val;
			uint32_t i = 0;
			yyjson_obj_iter_init(v, &iter);
			while ((key = yyjson_obj_iter_next(&iter))) {
				val = yyjson_obj_iter_get_val(key);
				node->pattern_properties[i].name = zend_string_init(yyjson_get_str(key), yyjson_get_len(key), 0);
				if (!jsonk_regex_is_valid(ZSTR_VAL(node->pattern_properties[i].name), ZSTR_LEN(node->pattern_properties[i].name))) {
					jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"patternProperties\" key is not a valid regular expression");
					node->pattern_properties_count = i;
					return false;
				}
				node->pattern_properties[i].schema = compile_node(val, ctx);
				if (!node->pattern_properties[i].schema) {
					node->pattern_properties_count = i;
					return false;
				}
				i++;
			}
		}
	}

	if ((v = yyjson_obj_get(schema, "propertyNames")) != NULL) {
		node->property_names = compile_node(v, ctx);
		if (!node->property_names) return false;
	}

	if ((v = yyjson_obj_get(schema, "dependentRequired")) != NULL) {
		if (!yyjson_is_obj(v)) {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"dependentRequired\" must be an object");
			return false;
		}
		node->dependent_required_count = (uint32_t) yyjson_obj_size(v);
		node->dependent_required = emalloc((node->dependent_required_count ? node->dependent_required_count : 1) * sizeof(jsonk_schema_dependent_required));
		{
			yyjson_obj_iter iter;
			yyjson_val *key, *val;
			uint32_t i = 0;
			yyjson_obj_iter_init(v, &iter);
			while ((key = yyjson_obj_iter_next(&iter))) {
				val = yyjson_obj_iter_get_val(key);
				if (!yyjson_is_arr(val)) {
					jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"dependentRequired\" entries must be arrays of strings");
					node->dependent_required_count = i;
					return false;
				}
				node->dependent_required[i].name = zend_string_init(yyjson_get_str(key), yyjson_get_len(key), 0);
				node->dependent_required[i].required_count = (uint32_t) yyjson_arr_size(val);
				node->dependent_required[i].required = emalloc((node->dependent_required[i].required_count ? node->dependent_required[i].required_count : 1) * sizeof(zend_string *));
				{
					yyjson_arr_iter iter2;
					yyjson_val *item;
					uint32_t j = 0;
					yyjson_arr_iter_init(val, &iter2);
					while ((item = yyjson_arr_iter_next(&iter2))) {
						if (!yyjson_is_str(item)) {
							node->dependent_required[i].required_count = j;
							node->dependent_required_count = i + 1; /* include this entry so its .name and partial .required[] are still reachable to free */
							jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"dependentRequired\" entries must be arrays of strings");
							return false;
						}
						node->dependent_required[i].required[j++] = zend_string_init(yyjson_get_str(item), yyjson_get_len(item), 0);
					}
				}
				i++;
			}
		}
	}

	if ((v = yyjson_obj_get(schema, "dependentSchemas")) != NULL) {
		if (!yyjson_is_obj(v)) {
			jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "\"dependentSchemas\" must be an object");
			return false;
		}
		node->dependent_schemas_count = (uint32_t) yyjson_obj_size(v);
		node->dependent_schemas = emalloc((node->dependent_schemas_count ? node->dependent_schemas_count : 1) * sizeof(jsonk_schema_property));
		{
			yyjson_obj_iter iter;
			yyjson_val *key, *val;
			uint32_t i = 0;
			yyjson_obj_iter_init(v, &iter);
			while ((key = yyjson_obj_iter_next(&iter))) {
				val = yyjson_obj_iter_get_val(key);
				node->dependent_schemas[i].name = zend_string_init(yyjson_get_str(key), yyjson_get_len(key), 0);
				node->dependent_schemas[i].schema = compile_node(val, ctx);
				if (!node->dependent_schemas[i].schema) {
					node->dependent_schemas_count = i;
					return false;
				}
				i++;
			}
		}
	}

	if ((v = yyjson_obj_get(schema, "unevaluatedProperties")) != NULL) {
		node->unevaluated_properties = compile_node(v, ctx);
		if (!node->unevaluated_properties) return false;
	}

	/* conditionals */
	if ((v = yyjson_obj_get(schema, "if")) != NULL) {
		node->if_schema = compile_node(v, ctx);
		if (!node->if_schema) return false;
	}
	if ((v = yyjson_obj_get(schema, "then")) != NULL) {
		node->then_schema = compile_node(v, ctx);
		if (!node->then_schema) return false;
	}
	if ((v = yyjson_obj_get(schema, "else")) != NULL) {
		node->else_schema = compile_node(v, ctx);
		if (!node->else_schema) return false;
	}

	/* combinators */
	if ((v = yyjson_obj_get(schema, "allOf")) != NULL && !compile_schema_array(v, ctx, &node->all_of, &node->all_of_count)) {
		return false;
	}
	if ((v = yyjson_obj_get(schema, "anyOf")) != NULL && !compile_schema_array(v, ctx, &node->any_of, &node->any_of_count)) {
		return false;
	}
	if ((v = yyjson_obj_get(schema, "oneOf")) != NULL && !compile_schema_array(v, ctx, &node->one_of, &node->one_of_count)) {
		return false;
	}
	if ((v = yyjson_obj_get(schema, "not")) != NULL) {
		node->not_schema = compile_node(v, ctx);
		if (!node->not_schema) return false;
	}

	return true;
}

static jsonk_schema_node *compile_node(yyjson_val *schema, jsonk_compile_ctx *ctx)
{
	jsonk_schema_node *node;
	yyjson_val *ref;

	if (yyjson_is_bool(schema)) {
		node = jsonk_schema_node_new(ctx);
		node->is_bool_schema = true;
		node->bool_schema_value = yyjson_get_bool(schema);
		return node;
	}

	if (!yyjson_is_obj(schema)) {
		jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "Each schema must be a JSON object or a boolean");
		return NULL;
	}

	ref = yyjson_obj_get(schema, "$ref");
	if (ref) {
		/* Sibling keywords alongside "$ref" are ignored -- see the
		 * struct doc comment in jsonk_schema.h. */
		return compile_ref(ref, ctx);
	}

	node = jsonk_schema_node_new(ctx);
	if (!fill_node_from_schema(node, schema, ctx)) {
		return NULL; /* `node` stays in the arena and is freed by jsonk_schema_compile()'s failure path */
	}
	return node;
}

jsonk_schema_node *jsonk_schema_compile(const char *schema, size_t schema_len)
{
	yyjson_read_err err;
	jsonk_compile_ctx ctx;
	jsonk_schema_node *root;

	memset(&ctx, 0, sizeof(ctx));

	ctx.root_doc = yyjson_read_opts((char *) schema, schema_len, YYJSON_READ_NOFLAG, NULL, &err);
	if (!ctx.root_doc) {
		jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "Invalid schema JSON: %s (at offset %zu)", err.msg, (size_t) err.pos);
		return NULL;
	}
	ctx.current_doc = ctx.root_doc;
	{
		/* The top-level schema's own base URI for resolving a RELATIVE
		 * "$ref" (see compile_ref()) -- its own "$id" if it declares
		 * one, or NULL (no known base at all -- the schema is just a raw
		 * string with no URL of its own, so a relative "$ref" at the top
		 * level has nothing to resolve against unless "$id" says
		 * otherwise). */
		yyjson_val *root_val = yyjson_doc_get_root(ctx.root_doc);
		yyjson_val *id_val = yyjson_is_obj(root_val) ? yyjson_obj_get(root_val, "$id") : NULL;
		if (id_val && yyjson_is_str(id_val)) {
			ctx.current_base_uri = zend_string_init(yyjson_get_str(id_val), yyjson_get_len(id_val), 0);
		}
	}
	zend_hash_init(&ctx.ref_cache, 8, NULL, NULL, 0); /* values are raw jsonk_schema_node* -- owned by the arena, not this table */

	root = compile_node(yyjson_doc_get_root(ctx.root_doc), &ctx);

	zend_hash_destroy(&ctx.ref_cache);
	yyjson_doc_free(ctx.root_doc);
	if (ctx.current_base_uri) zend_string_release(ctx.current_base_uri);

	if (!root) {
		uint32_t i;
		/* Shallow-free every node allocated so far (see
		 * jsonk_schema_free() for why this can't be a recursive
		 * child-pointer walk once "$ref" is in the picture). */
		for (i = 0; i < ctx.arena_count; i++) {
			jsonk_schema_node_free_shallow(ctx.arena[i]);
		}
		if (ctx.arena) efree(ctx.arena);
		return NULL;
	}

	root->arena = ctx.arena;
	root->arena_count = ctx.arena_count;
	return root;
}

/* Frees everything a single node owns directly (copied strings/zvals, and
 * the pointer ARRAYS it holds) but deliberately does NOT recurse into any
 * jsonk_schema_node* it points to -- those are separate entries in the
 * arena, freed on their own turn by jsonk_schema_free(). This is what
 * makes freeing safe once "$ref" can make several places share the same
 * node, or a schema can reference itself. */
static void jsonk_schema_node_free_shallow(jsonk_schema_node *node)
{
	uint32_t i;

	if (node->is_bool_schema) {
		efree(node);
		return;
	}

	if (node->enum_values) {
		for (i = 0; i < node->enum_count; i++) zval_ptr_dtor(&node->enum_values[i]);
		efree(node->enum_values);
	}
	if (node->has_const) zval_ptr_dtor(&node->const_value);
	if (node->pattern) zend_string_release(node->pattern);
	if (node->format) zend_string_release(node->format);

	if (node->prefix_items) efree(node->prefix_items);

	if (node->properties) {
		for (i = 0; i < node->properties_count; i++) zend_string_release(node->properties[i].name);
		efree(node->properties);
	}
	if (node->required) {
		for (i = 0; i < node->required_count; i++) zend_string_release(node->required[i]);
		efree(node->required);
	}
	if (node->pattern_properties) {
		for (i = 0; i < node->pattern_properties_count; i++) zend_string_release(node->pattern_properties[i].name);
		efree(node->pattern_properties);
	}
	if (node->dependent_required) {
		for (i = 0; i < node->dependent_required_count; i++) {
			uint32_t j;
			zend_string_release(node->dependent_required[i].name);
			for (j = 0; j < node->dependent_required[i].required_count; j++) {
				zend_string_release(node->dependent_required[i].required[j]);
			}
			efree(node->dependent_required[i].required);
		}
		efree(node->dependent_required);
	}
	if (node->dependent_schemas) {
		for (i = 0; i < node->dependent_schemas_count; i++) zend_string_release(node->dependent_schemas[i].name);
		efree(node->dependent_schemas);
	}

	if (node->all_of) efree(node->all_of);
	if (node->any_of) efree(node->any_of);
	if (node->one_of) efree(node->one_of);

	efree(node);
}

void jsonk_schema_free(jsonk_schema_node *root)
{
	jsonk_schema_node **arena;
	uint32_t i, count;

	if (!root) return;

	/* Capture these before freeing -- `root` itself is one of the arena
	 * entries and gets freed inside the loop below, so `root->arena`/
	 * `root->arena_count` must not be read again afterward. */
	arena = root->arena;
	count = root->arena_count;

	for (i = 0; i < count; i++) {
		jsonk_schema_node_free_shallow(arena[i]);
	}
	if (arena) efree(arena);
}

/* ========================================================================
 * Shared helpers
 * ======================================================================== */

void jsonk_path_push(smart_str *path, const char *segment, size_t segment_len)
{
	size_t i;
	smart_str_appendc(path, '/');
	for (i = 0; i < segment_len; i++) {
		if (segment[i] == '~') {
			smart_str_appendl(path, "~0", 2);
		} else if (segment[i] == '/') {
			smart_str_appendl(path, "~1", 2);
		} else {
			smart_str_appendc(path, segment[i]);
		}
	}
}

void jsonk_path_push_index(smart_str *path, size_t index)
{
	smart_str_appendc(path, '/');
	smart_str_append_long(path, (zend_long) index);
}

/* smart_str's own "pop a segment" idiom (used throughout this file) is
 * `ZSTR_LEN(path->s) = saved_len;` -- cheap (no reallocation), but it only
 * shrinks the LOGICAL length, it doesn't touch the byte that used to be
 * the NUL terminator before the segment was appended. Reading the buffer
 * as a plain C string (jsonk_add_violation() does, via add_assoc_string(),
 * which uses strlen()) without re-terminating it here first would walk
 * straight past the truncation point into whatever longer content used
 * to occupy that same buffer -- a real, confirmed bug (path values coming
 * out with a stale fragment of an earlier, longer message appended).
 * smart_str_0() writes the terminator at the CURRENT length without
 * changing it; always safe since smart_str never shrinks its actual
 * allocation, only ZSTR_LEN. */
static const char *current_path(smart_str *path)
{
	if (!path->s) return "";
	smart_str_0(path);
	return ZSTR_VAL(path->s);
}

/* Looks up a key taken from another table. An object's property table keeps
 * numeric names as strings ("0") while an array stores them as integers, so
 * each form falls back to the other. */
static zval *jsonk_table_find(HashTable *ht, zend_string *key, zend_ulong idx)
{
	zval *found;

	if (key) {
		found = zend_hash_find_ind(ht, key);
		return found ? found : zend_symtable_find(ht, key);
	}
	found = zend_hash_index_find(ht, idx);
	if (!found) {
		char buf[MAX_LENGTH_OF_LONG + 1];
		char *end = buf + sizeof(buf) - 1;
		char *start = zend_print_ulong_to_buf(end, idx);
		found = zend_hash_str_find_ind(ht, start, end - start);
	}
	return found;
}

/* Same size and every key of `ha` maps to an equal value in `hb`. The _IND
 * iteration looks through object property tables' INDIRECT slots. */
static bool jsonk_tables_equal(HashTable *ha, HashTable *hb)
{
	zend_string *key;
	zend_ulong idx;
	zval *va;

	if (zend_hash_num_elements(ha) != zend_hash_num_elements(hb)) {
		return false;
	}

	ZEND_HASH_FOREACH_KEY_VAL_IND(ha, idx, key, va) {
		zval *vb = jsonk_table_find(hb, key, idx);
		if (!vb || !jsonk_values_equal(va, vb)) {
			return false;
		}
	} ZEND_HASH_FOREACH_END();
	return true;
}

/* A PHP array standing for a JSON object (JSON_OBJECT_AS_ARRAY decoding, or
 * an associative array given to jsonk_encode()): anything but a non-empty
 * list. An empty array is ambiguous and counts as both. */
static bool jsonk_array_is_object_like(HashTable *ht)
{
	return zend_hash_num_elements(ht) == 0 || !zend_array_is_list(ht);
}

bool jsonk_values_equal(zval *a, zval *b)
{
	if ((Z_TYPE_P(a) == IS_LONG || Z_TYPE_P(a) == IS_DOUBLE) &&
		(Z_TYPE_P(b) == IS_LONG || Z_TYPE_P(b) == IS_DOUBLE)) {
		/* "1 and 1.0 are considered equal" -- JSON Schema spec's equality
		 * definition for enum/const, regardless of PHP's int-vs-float. */
		double x = (Z_TYPE_P(a) == IS_LONG) ? (double) Z_LVAL_P(a) : Z_DVAL_P(a);
		double y = (Z_TYPE_P(b) == IS_LONG) ? (double) Z_LVAL_P(b) : Z_DVAL_P(b);
		return x == y;
	}

	if (Z_TYPE_P(a) == IS_OBJECT && Z_TYPE_P(b) == IS_ARRAY) {
		return jsonk_array_is_object_like(Z_ARRVAL_P(b)) && jsonk_tables_equal(Z_OBJPROP_P(a), Z_ARRVAL_P(b));
	}
	if (Z_TYPE_P(a) == IS_ARRAY && Z_TYPE_P(b) == IS_OBJECT) {
		return jsonk_array_is_object_like(Z_ARRVAL_P(a)) && jsonk_tables_equal(Z_ARRVAL_P(a), Z_OBJPROP_P(b));
	}

	if (Z_TYPE_P(a) != Z_TYPE_P(b)) {
		return false;
	}

	switch (Z_TYPE_P(a)) {
		case IS_NULL:
		case IS_TRUE:
		case IS_FALSE:
			return true;
		case IS_STRING:
			return zend_string_equals(Z_STR_P(a), Z_STR_P(b));
		case IS_ARRAY:
			return jsonk_tables_equal(Z_ARRVAL_P(a), Z_ARRVAL_P(b));
		case IS_OBJECT:
			/* A JSON object decodes to stdClass: compare its properties
			 * (without this, const/enum/uniqueItems never matched objects). */
			return jsonk_tables_equal(Z_OBJPROP_P(a), Z_OBJPROP_P(b));
		default:
			return false;
	}
}

/* UTF-8 codepoint count -- JSON Schema's "minLength"/"maxLength" count
 * Unicode characters, not bytes. */
static size_t jsonk_utf8_len(const char *s, size_t byte_len)
{
	size_t count = 0, i = 0;
	while (i < byte_len) {
		unsigned char c = (unsigned char) s[i];
		if ((c & 0x80) == 0x00)      i += 1;
		else if ((c & 0xE0) == 0xC0) i += 2;
		else if ((c & 0xF0) == 0xE0) i += 3;
		else if ((c & 0xF8) == 0xF0) i += 4;
		else                          i += 1; /* invalid lead byte -- count as one to avoid looping forever */
		count++;
	}
	return count;
}

static void merge_evaluated(HashTable *dest, HashTable *src)
{
	zend_string *key;
	zend_ulong idx;
	zval *val;

	if (!dest || !src) return;

	ZEND_HASH_FOREACH_KEY_VAL(src, idx, key, val) {
		if (key) {
			zend_hash_add_empty_element(dest, key);
		} else {
			zend_hash_index_add_empty_element(dest, idx);
		}
	} ZEND_HASH_FOREACH_END();
}

/* Runs `schema` against `value` purely to find out whether it matches and
 * what it would evaluate, WITHOUT the result affecting the caller's own
 * violation list (used by anyOf/oneOf/if to probe a branch) unless
 * `record_violations` is true (used by allOf, whose failing branches
 * SHOULD surface their real violations directly -- see docs/DECISIONS.md). On a
 * match, and if `merge_props`/`merge_items` are non-NULL, merges what the
 * branch evaluated into them (annotations from a non-matching branch are
 * discarded, per spec). Returns whether `schema` matched. */
static bool probe_branch(
	jsonk_schema_node *schema, zval *value, smart_str *path, zend_long depth_remaining,
	bool record_violations, HashTable *merge_props, HashTable *merge_items
) {
	HashTable branch_props, branch_items;
	bool matched;

	zend_hash_init(&branch_props, 8, NULL, NULL, 0);
	zend_hash_init(&branch_items, 8, NULL, NULL, 0);

	if (!record_violations) jsonk_suppress_violations_push();
	matched = jsonk_schema_validate_zval(schema, value, path, depth_remaining, &branch_props, &branch_items);
	if (!record_violations) jsonk_suppress_violations_pop();

	if (matched) {
		merge_evaluated(merge_props, &branch_props);
		merge_evaluated(merge_items, &branch_items);
	}

	zend_hash_destroy(&branch_props);
	zend_hash_destroy(&branch_items);

	return matched;
}

/* ========================================================================
 * The validator.
 * ======================================================================== */

bool jsonk_schema_validate_zval(
	jsonk_schema_node *schema, zval *value, smart_str *path, zend_long depth_remaining,
	HashTable *evaluated_props_out, HashTable *evaluated_items_out
) {
	bool ok = true;
	uint32_t actual_type;
	bool is_list = false;
	HashTable own_props, own_items;
	bool is_object_like, is_array_like;

	/* Same IS_INDIRECT concern as jsonk_encode.c's build_val() -- a zval
	 * pulled from an object's property table (via zend_hash_find() on
	 * Z_OBJPROP_P(), e.g. the "properties" keyword validating a declared
	 * property) can be IS_INDIRECT rather than the real value directly.
	 * Confirmed as a real bug by an actual runtime test. */
	if (Z_ISREF_P(value)) value = Z_REFVAL_P(value);
	if (Z_TYPE_P(value) == IS_INDIRECT) {
		value = Z_INDIRECT_P(value);
		if (Z_ISREF_P(value)) value = Z_REFVAL_P(value);
	}

	if (depth_remaining <= 0) {
		jsonk_set_error(JSONK_ERROR_DEPTH, "Maximum schema validation depth exceeded (a deeply nested value, or a \"$ref\" cycle)");
		return false;
	}

	if (schema->is_bool_schema) {
		if (!schema->bool_schema_value) {
			jsonk_add_violation(current_path(path), "false", "No value is allowed here");
			return false;
		}
		return true;
	}

	if (schema->ref_target) {
		return jsonk_schema_validate_zval(schema->ref_target, value, path, depth_remaining - 1, evaluated_props_out, evaluated_items_out);
	}

	switch (Z_TYPE_P(value)) {
		case IS_NULL:
			actual_type = JSONK_TYPE_NULL;
			break;
		case IS_TRUE:
		case IS_FALSE:
			actual_type = JSONK_TYPE_BOOL;
			break;
		case IS_LONG:
			actual_type = JSONK_TYPE_INT | JSONK_TYPE_NUMBER;
			break;
		case IS_DOUBLE: {
			double d = Z_DVAL_P(value);
			actual_type = JSONK_TYPE_NUMBER;
			if (!zend_isnan(d) && !zend_isinf(d) && d == (double) (zend_long) d) {
				actual_type |= JSONK_TYPE_INT;
			}
			break;
		}
		case IS_STRING:
			actual_type = JSONK_TYPE_STRING;
			break;
		case IS_ARRAY:
			is_list = zend_array_is_list(Z_ARRVAL_P(value));
			actual_type = is_list ? JSONK_TYPE_ARRAY : JSONK_TYPE_OBJECT;
			break;
		case IS_OBJECT:
			actual_type = JSONK_TYPE_OBJECT;
			break;
		default:
			jsonk_add_violation(current_path(path), "type", "Value of an unsupported PHP type cannot be validated");
			return false;
	}

	if (schema->type_mask != JSONK_TYPE_ANY && !(schema->type_mask & actual_type)) {
		jsonk_add_violation(current_path(path), "type", "Value does not match the schema's expected type");
		ok = false;
	}

	if (schema->has_enum) {
		uint32_t i;
		bool found = false;
		for (i = 0; i < schema->enum_count; i++) {
			if (jsonk_values_equal(value, &schema->enum_values[i])) { found = true; break; }
		}
		if (!found) {
			jsonk_add_violation(current_path(path), "enum", "Value is not one of the allowed enum values");
			ok = false;
		}
	}

	if (schema->has_const && !jsonk_values_equal(value, &schema->const_value)) {
		jsonk_add_violation(current_path(path), "const", "Value does not equal the schema's const value");
		ok = false;
	}

	if (Z_TYPE_P(value) == IS_STRING) {
		size_t len = jsonk_utf8_len(Z_STRVAL_P(value), Z_STRLEN_P(value));
		if (schema->has_min_length && len < schema->min_length) {
			jsonk_add_violation(current_path(path), "minLength", "String is shorter than minLength (%u)", schema->min_length);
			ok = false;
		}
		if (schema->has_max_length && len > schema->max_length) {
			jsonk_add_violation(current_path(path), "maxLength", "String is longer than maxLength (%u)", schema->max_length);
			ok = false;
		}
		if (schema->pattern && !jsonk_regex_match(ZSTR_VAL(schema->pattern), ZSTR_LEN(schema->pattern), Z_STRVAL_P(value), Z_STRLEN_P(value))) {
			jsonk_add_violation(current_path(path), "pattern", "String does not match pattern \"%s\"", ZSTR_VAL(schema->pattern));
			ok = false;
		}
		if (schema->format && !jsonk_check_format(ZSTR_VAL(schema->format), ZSTR_LEN(schema->format), Z_STRVAL_P(value), Z_STRLEN_P(value))) {
			jsonk_add_violation(current_path(path), "format", "String does not satisfy format \"%s\"", ZSTR_VAL(schema->format));
			ok = false;
		}
	}

	if (actual_type & JSONK_TYPE_NUMBER) {
		double num = (Z_TYPE_P(value) == IS_LONG) ? (double) Z_LVAL_P(value) : Z_DVAL_P(value);
		if (schema->has_minimum) {
			bool violated = schema->exclusive_minimum ? (num <= schema->minimum) : (num < schema->minimum);
			if (violated) {
				jsonk_add_violation(current_path(path), schema->exclusive_minimum ? "exclusiveMinimum" : "minimum", "Number is below the allowed minimum");
				ok = false;
			}
		}
		if (schema->has_maximum) {
			bool violated = schema->exclusive_maximum ? (num >= schema->maximum) : (num > schema->maximum);
			if (violated) {
				jsonk_add_violation(current_path(path), schema->exclusive_maximum ? "exclusiveMaximum" : "maximum", "Number is above the allowed maximum");
				ok = false;
			}
		}
		if (schema->has_multiple_of && schema->multiple_of != 0) {
			double q = num / schema->multiple_of;
			if (fabs(q - round(q)) > 1e-9) {
				jsonk_add_violation(current_path(path), "multipleOf", "Number is not a multiple of %g", schema->multiple_of);
				ok = false;
			}
		}
	}

	is_array_like = (Z_TYPE_P(value) == IS_ARRAY && is_list);
	is_object_like = (Z_TYPE_P(value) == IS_ARRAY && !is_list) || Z_TYPE_P(value) == IS_OBJECT;

	zend_hash_init(&own_props, 8, NULL, NULL, 0);
	zend_hash_init(&own_items, 8, NULL, NULL, 0);

	if (is_array_like) {
		HashTable *ht = Z_ARRVAL_P(value);
		uint32_t count = zend_hash_num_elements(ht);

		if (schema->has_min_items && count < schema->min_items) {
			jsonk_add_violation(current_path(path), "minItems", "Array has fewer than minItems (%u) elements", schema->min_items);
			ok = false;
		}
		if (schema->has_max_items && count > schema->max_items) {
			jsonk_add_violation(current_path(path), "maxItems", "Array has more than maxItems (%u) elements", schema->max_items);
			ok = false;
		}
		if (schema->unique_items && count > 1) {
			zval *va;
			zend_ulong ia = 0;
			bool dup = false;
			ZEND_HASH_FOREACH_VAL(ht, va) {
				zval *vb;
				zend_ulong ib = 0;
				ZEND_HASH_FOREACH_VAL(ht, vb) {
					if (ib > ia && jsonk_values_equal(va, vb)) { dup = true; break; }
					ib++;
				} ZEND_HASH_FOREACH_END();
				if (dup) break;
				ia++;
			} ZEND_HASH_FOREACH_END();
			if (dup) {
				jsonk_add_violation(current_path(path), "uniqueItems", "Array elements are not all unique");
				ok = false;
			}
		}

		{
			zval *item;
			zend_ulong idx = 0;
			ZEND_HASH_FOREACH_VAL(ht, item) {
				jsonk_schema_node *item_schema = NULL;
				if (idx < schema->prefix_items_count) {
					item_schema = schema->prefix_items[idx];
				} else if (schema->items) {
					item_schema = schema->items;
				}
				if (item_schema) {
					size_t saved = path->s ? ZSTR_LEN(path->s) : 0;
					jsonk_path_push_index(path, (size_t) idx);
					if (!jsonk_schema_validate_zval(item_schema, item, path, depth_remaining - 1, NULL, NULL)) ok = false;
					if (path->s) ZSTR_LEN(path->s) = saved;
					zend_hash_index_add_empty_element(&own_items, idx);
				}
				idx++;
			} ZEND_HASH_FOREACH_END();
		}

		if (schema->contains) {
			zval *item;
			zend_ulong idx = 0;
			uint32_t match_count = 0;
			ZEND_HASH_FOREACH_VAL(ht, item) {
				bool matched;
				jsonk_suppress_violations_push();
				matched = jsonk_schema_validate_zval(schema->contains, item, path, depth_remaining - 1, NULL, NULL);
				jsonk_suppress_violations_pop();
				if (matched) {
					match_count++;
					zend_hash_index_add_empty_element(&own_items, idx);
				}
				idx++;
			} ZEND_HASH_FOREACH_END();
			if (match_count < schema->min_contains) {
				jsonk_add_violation(current_path(path), "contains", "Array has fewer than minContains (%u) matching items", schema->min_contains);
				ok = false;
			}
			if (schema->has_max_contains && match_count > schema->max_contains) {
				jsonk_add_violation(current_path(path), "maxContains", "Array has more than maxContains (%u) matching items", schema->max_contains);
				ok = false;
			}
		}
	}

	if (is_object_like) {
		HashTable *ht = (Z_TYPE_P(value) == IS_ARRAY) ? Z_ARRVAL_P(value) : Z_OBJPROP_P(value);
		uint32_t count = zend_hash_num_elements(ht);
		uint32_t i;

		if (schema->has_min_properties && count < schema->min_properties) {
			jsonk_add_violation(current_path(path), "minProperties", "Object has fewer than minProperties (%u) members", schema->min_properties);
			ok = false;
		}
		if (schema->has_max_properties && count > schema->max_properties) {
			jsonk_add_violation(current_path(path), "maxProperties", "Object has more than maxProperties (%u) members", schema->max_properties);
			ok = false;
		}

		for (i = 0; i < schema->required_count; i++) {
			if (!zend_hash_exists(ht, schema->required[i])) {
				jsonk_add_violation(current_path(path), "required", "Missing required property \"%s\"", ZSTR_VAL(schema->required[i]));
				ok = false;
			}
		}

		for (i = 0; i < schema->properties_count; i++) {
			zval *prop = zend_hash_find(ht, schema->properties[i].name);
			if (prop) {
				size_t saved = path->s ? ZSTR_LEN(path->s) : 0;
				jsonk_path_push(path, ZSTR_VAL(schema->properties[i].name), ZSTR_LEN(schema->properties[i].name));
				if (!jsonk_schema_validate_zval(schema->properties[i].schema, prop, path, depth_remaining - 1, NULL, NULL)) ok = false;
				if (path->s) ZSTR_LEN(path->s) = saved;
				zend_hash_add_empty_element(&own_props, schema->properties[i].name);
			}
		}

		if (schema->pattern_properties_count) {
			zend_string *key;
			zval *val;
			ZEND_HASH_FOREACH_STR_KEY_VAL(ht, key, val) {
				uint32_t j;
				if (!key) continue;
				for (j = 0; j < schema->pattern_properties_count; j++) {
					if (jsonk_regex_match(ZSTR_VAL(schema->pattern_properties[j].name), ZSTR_LEN(schema->pattern_properties[j].name), ZSTR_VAL(key), ZSTR_LEN(key))) {
						size_t saved = path->s ? ZSTR_LEN(path->s) : 0;
						jsonk_path_push(path, ZSTR_VAL(key), ZSTR_LEN(key));
						if (!jsonk_schema_validate_zval(schema->pattern_properties[j].schema, val, path, depth_remaining - 1, NULL, NULL)) ok = false;
						if (path->s) ZSTR_LEN(path->s) = saved;
						zend_hash_add_empty_element(&own_props, key);
					}
				}
			} ZEND_HASH_FOREACH_END();
		}

		if (schema->additional_properties) {
			zend_string *key;
			zval *val;
			ZEND_HASH_FOREACH_STR_KEY_VAL(ht, key, val) {
				if (!key) continue;
				/* own_props already holds every key the "properties"/
				 * "patternProperties" loops above matched (they both ran
				 * before this point), so checking it alone is enough to
				 * know whether `key` still needs additionalProperties. */
				if (zend_hash_exists(&own_props, key)) continue;

				if (schema->additional_properties->is_bool_schema && !schema->additional_properties->bool_schema_value) {
					jsonk_add_violation(current_path(path), "additionalProperties", "Unexpected property \"%s\"", ZSTR_VAL(key));
					ok = false;
				} else {
					size_t saved = path->s ? ZSTR_LEN(path->s) : 0;
					jsonk_path_push(path, ZSTR_VAL(key), ZSTR_LEN(key));
					if (!jsonk_schema_validate_zval(schema->additional_properties, val, path, depth_remaining - 1, NULL, NULL)) ok = false;
					if (path->s) ZSTR_LEN(path->s) = saved;
				}
				zend_hash_add_empty_element(&own_props, key);
			} ZEND_HASH_FOREACH_END();
		}

		if (schema->property_names) {
			zend_string *key;
			ZEND_HASH_FOREACH_STR_KEY(ht, key) {
				zval key_zv;
				if (!key) continue;
				ZVAL_STR_COPY(&key_zv, key);
				if (!jsonk_schema_validate_zval(schema->property_names, &key_zv, path, depth_remaining - 1, NULL, NULL)) ok = false;
				zval_ptr_dtor(&key_zv);
			} ZEND_HASH_FOREACH_END();
		}

		for (i = 0; i < schema->dependent_required_count; i++) {
			if (zend_hash_exists(ht, schema->dependent_required[i].name)) {
				uint32_t j;
				for (j = 0; j < schema->dependent_required[i].required_count; j++) {
					if (!zend_hash_exists(ht, schema->dependent_required[i].required[j])) {
						jsonk_add_violation(current_path(path), "dependentRequired",
							"Property \"%s\" requires \"%s\" to also be present",
							ZSTR_VAL(schema->dependent_required[i].name), ZSTR_VAL(schema->dependent_required[i].required[j]));
						ok = false;
					}
				}
			}
		}

		for (i = 0; i < schema->dependent_schemas_count; i++) {
			if (zend_hash_exists(ht, schema->dependent_schemas[i].name)) {
				if (!probe_branch(schema->dependent_schemas[i].schema, value, path, depth_remaining - 1, true, &own_props, &own_items)) {
					ok = false;
				}
			}
		}
	}

	if (schema->if_schema) {
		bool if_matched = probe_branch(schema->if_schema, value, path, depth_remaining - 1, false, NULL, NULL);
		if (if_matched && schema->then_schema) {
			if (!probe_branch(schema->then_schema, value, path, depth_remaining - 1, true, &own_props, &own_items)) ok = false;
		} else if (!if_matched && schema->else_schema) {
			if (!probe_branch(schema->else_schema, value, path, depth_remaining - 1, true, &own_props, &own_items)) ok = false;
		}
	}

	if (schema->all_of_count) {
		uint32_t i;
		for (i = 0; i < schema->all_of_count; i++) {
			if (!probe_branch(schema->all_of[i], value, path, depth_remaining - 1, true, &own_props, &own_items)) ok = false;
		}
	}
	if (schema->any_of_count) {
		uint32_t i;
		bool matched = false;
		for (i = 0; i < schema->any_of_count; i++) {
			if (probe_branch(schema->any_of[i], value, path, depth_remaining - 1, false, &own_props, &own_items)) {
				matched = true;
			}
		}
		if (!matched) {
			jsonk_add_violation(current_path(path), "anyOf", "Value does not match any of the allowed schemas");
			ok = false;
		}
	}
	if (schema->one_of_count) {
		uint32_t i, match_count = 0;
		for (i = 0; i < schema->one_of_count; i++) {
			if (probe_branch(schema->one_of[i], value, path, depth_remaining - 1, false, &own_props, &own_items)) {
				match_count++;
			}
		}
		if (match_count != 1) {
			jsonk_add_violation(current_path(path), "oneOf", "Value must match exactly one of the allowed schemas (matched %u)", match_count);
			ok = false;
		}
	}
	if (schema->not_schema) {
		/* "not"'s annotations are always discarded, win or lose -- pass
		 * NULL,NULL rather than own_props/own_items. */
		if (probe_branch(schema->not_schema, value, path, depth_remaining - 1, false, NULL, NULL)) {
			jsonk_add_violation(current_path(path), "not", "Value must not match the \"not\" schema");
			ok = false;
		}
	}

	if (is_array_like && schema->unevaluated_items) {
		HashTable *ht = Z_ARRVAL_P(value);
		zval *item;
		zend_ulong idx = 0;
		ZEND_HASH_FOREACH_VAL(ht, item) {
			if (!zend_hash_index_exists(&own_items, idx)) {
				if (schema->unevaluated_items->is_bool_schema && !schema->unevaluated_items->bool_schema_value) {
					jsonk_add_violation(current_path(path), "unevaluatedItems", "Unexpected item at index %u", (unsigned int) idx);
					ok = false;
				} else {
					size_t saved = path->s ? ZSTR_LEN(path->s) : 0;
					jsonk_path_push_index(path, (size_t) idx);
					if (!jsonk_schema_validate_zval(schema->unevaluated_items, item, path, depth_remaining - 1, NULL, NULL)) ok = false;
					if (path->s) ZSTR_LEN(path->s) = saved;
				}
				zend_hash_index_add_empty_element(&own_items, idx);
			}
			idx++;
		} ZEND_HASH_FOREACH_END();
	}

	if (is_object_like && schema->unevaluated_properties) {
		HashTable *ht = (Z_TYPE_P(value) == IS_ARRAY) ? Z_ARRVAL_P(value) : Z_OBJPROP_P(value);
		zend_string *key;
		zval *val;
		ZEND_HASH_FOREACH_STR_KEY_VAL(ht, key, val) {
			if (!key) continue;
			if (!zend_hash_exists(&own_props, key)) {
				if (schema->unevaluated_properties->is_bool_schema && !schema->unevaluated_properties->bool_schema_value) {
					jsonk_add_violation(current_path(path), "unevaluatedProperties", "Unexpected property \"%s\"", ZSTR_VAL(key));
					ok = false;
				} else {
					size_t saved = path->s ? ZSTR_LEN(path->s) : 0;
					jsonk_path_push(path, ZSTR_VAL(key), ZSTR_LEN(key));
					if (!jsonk_schema_validate_zval(schema->unevaluated_properties, val, path, depth_remaining - 1, NULL, NULL)) ok = false;
					if (path->s) ZSTR_LEN(path->s) = saved;
				}
				zend_hash_add_empty_element(&own_props, key);
			}
		} ZEND_HASH_FOREACH_END();
	}

	merge_evaluated(evaluated_props_out, &own_props);
	merge_evaluated(evaluated_items_out, &own_items);
	zend_hash_destroy(&own_props);
	zend_hash_destroy(&own_items);

	return ok;
}
