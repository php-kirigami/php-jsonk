#ifndef JSONK_SCHEMA_H
#define JSONK_SCHEMA_H

#include "php_jsonk.h"
#include <zend_smart_str.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Compiled JSON Schema representation -- aims for full draft 2020-12
 * keyword coverage for a SELF-CONTAINED schema document (no network
 * fetch: "$ref" only resolves same-document JSON Pointers, see
 * jsonk_schema_compile()). A schema is compiled once per
 * jsonk_decode()/jsonk_encode()/jsonk_validate() call (no caching/reuse
 * across calls yet -- see docs/TODO.md) from the raw
 * `$schema` argument, parsed via yyjson's read API, then walked against
 * either a zval tree (encode, and decode's post-build validation pass --
 * see docs/DECISIONS.md's "two passes, not fused" decision).
 * ======================================================================== */

typedef struct jsonk_schema_node jsonk_schema_node;

#define JSONK_TYPE_ANY    0u
#define JSONK_TYPE_NULL   (1u << 0)
#define JSONK_TYPE_BOOL   (1u << 1)
#define JSONK_TYPE_INT    (1u << 2) /* "integer": a number with zero fractional part */
#define JSONK_TYPE_NUMBER (1u << 3) /* "number": any int or float */
#define JSONK_TYPE_STRING (1u << 4)
#define JSONK_TYPE_ARRAY  (1u << 5)
#define JSONK_TYPE_OBJECT (1u << 6)

typedef struct {
	zend_string *name;         /* a property name (properties/dependentSchemas) or a raw regex pattern (patternProperties) */
	jsonk_schema_node *schema;
} jsonk_schema_property;

typedef struct {
	zend_string *name;             /* triggering property name */
	zend_string **required;
	uint32_t required_count;
} jsonk_schema_dependent_required;

struct jsonk_schema_node {
	/* JSON Schema's boolean-schema shorthand: `true` (anything valid) or
	 * `false` (nothing valid) used as a whole (sub-)schema, e.g.
	 * "additionalProperties": false. When set, every other field below is
	 * unused. */
	bool is_bool_schema;
	bool bool_schema_value;

	/* "$ref": when set, this node is a pure indirection to another
	 * compiled node -- every field below is unused and validation just
	 * recurses into ref_target with the same value/path. Sibling
	 * keywords alongside "$ref" are ignored by design (see
	 * jsonk_schema_compile()'s doc comment: real-world schemas
	 * overwhelmingly use "$ref" alone; 2019-09+'s "$ref-with-siblings" is
	 * a known, deliberate gap). */
	jsonk_schema_node *ref_target;

	uint32_t type_mask; /* JSONK_TYPE_ANY (0) = no "type" keyword, anything goes */

	bool has_enum;
	zval *enum_values; /* emalloc'd array of `enum_count` zvals, each a copy */
	uint32_t enum_count;

	bool has_const;
	zval const_value;

	/* string */
	bool has_min_length; uint32_t min_length;
	bool has_max_length; uint32_t max_length;
	zend_string *pattern; /* raw regex text (no delimiters) -- see jsonk_regex.h; NULL = keyword absent */
	zend_string *format;  /* raw format name -- see jsonk_format.h; NULL = keyword absent */

	/* number / integer */
	bool has_minimum; double minimum; bool exclusive_minimum;
	bool has_maximum; double maximum; bool exclusive_maximum;
	bool has_multiple_of; double multiple_of;

	/* array */
	jsonk_schema_node *items;         /* draft-2020-12 semantics: applies from index `prefix_items_count` onward (or index 0 if prefixItems is absent) */
	jsonk_schema_node **prefix_items; /* tuple validation; NULL if absent */
	uint32_t prefix_items_count;
	bool has_min_items; uint32_t min_items;
	bool has_max_items; uint32_t max_items;
	bool unique_items;
	jsonk_schema_node *contains;
	bool has_min_contains; uint32_t min_contains; /* default 1 when `contains` is present and this is absent */
	bool has_max_contains; uint32_t max_contains; /* default unbounded */
	jsonk_schema_node *unevaluated_items;         /* NULL = keyword absent */

	/* object */
	jsonk_schema_property *properties;
	uint32_t properties_count;
	zend_string **required;
	uint32_t required_count;
	/* NULL = keyword absent (default: additional properties allowed). A
	 * compiled `false` bool-schema means "no additional properties"; any
	 * other compiled node is a real sub-schema additional properties must
	 * satisfy. */
	jsonk_schema_node *additional_properties;
	bool has_min_properties; uint32_t min_properties;
	bool has_max_properties; uint32_t max_properties;
	jsonk_schema_property *pattern_properties; /* .name = raw regex pattern */
	uint32_t pattern_properties_count;
	jsonk_schema_node *property_names; /* applied to every key as a string instance; NULL = keyword absent */
	jsonk_schema_dependent_required *dependent_required;
	uint32_t dependent_required_count;
	jsonk_schema_property *dependent_schemas; /* .name = triggering property */
	uint32_t dependent_schemas_count;
	jsonk_schema_node *unevaluated_properties; /* NULL = keyword absent */

	/* conditionals */
	jsonk_schema_node *if_schema;
	jsonk_schema_node *then_schema;
	jsonk_schema_node *else_schema;

	/* combinators */
	jsonk_schema_node **all_of; uint32_t all_of_count;
	jsonk_schema_node **any_of; uint32_t any_of_count;
	jsonk_schema_node **one_of; uint32_t one_of_count;
	jsonk_schema_node *not_schema;

	/* Set ONLY on the node returned by jsonk_schema_compile() (NULL on
	 * every other node). Because "$ref" can make the compiled schema a
	 * DAG (or even contain cycles, for self-referential schemas) rather
	 * than a strict tree, ownership/freeing can't be a simple recursive
	 * child-pointer walk -- jsonk_schema_free() instead frees every node
	 * ever allocated during this compile from this flat list, exactly
	 * once each, regardless of how many places point to it. */
	jsonk_schema_node **arena;
	uint32_t arena_count;
};

/* Compiles a raw JSON Schema document (`schema`/`schema_len`) into a tree.
 * Returns NULL and calls jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, ...)
 * on malformed JSON, an unsupported schema shape, or a "$ref" this
 * implementation can't resolve (anything other than a same-document
 * fragment, "#" or "#/json/pointer" -- no network fetch, see the struct
 * doc comment above). The result must be freed with jsonk_schema_free(). */
jsonk_schema_node *jsonk_schema_compile(const char *schema, size_t schema_len);
void jsonk_schema_free(jsonk_schema_node *node);

/* The validator. Walks an already-built zval tree against a compiled
 * schema, reporting violations via jsonk_add_violation(). Returns true
 * iff no violation was found. `path` is the JSON-Pointer path of `value`
 * so far (empty string at the root). `depth_remaining` is decremented on
 * every recursive step (array items, object properties, "$ref"
 * indirection, combinator branches -- not just container nesting), the
 * same $depth the caller passed to jsonk_decode()/jsonk_encode(), so a
 * pathological schema (e.g. a "$ref" cycle with no other constraint) or a
 * genuinely deep value can't blow the C stack.
 *
 * `evaluated_props`/`evaluated_items` are optional (pass NULL when the
 * caller doesn't need them): when non-NULL and `value` is an
 * object/assoc-array or a list array respectively, this call ADDS every
 * property key / item index it (and any nested allOf branch, the
 * matching anyOf/oneOf branch(es), and the taken if/then or if/else
 * branch) evaluated -- used internally to resolve "unevaluatedProperties"/
 * "unevaluatedItems", and exposed so a parent combinator can merge a
 * branch's evaluated set into its own. Top-level callers (jsonk.c,
 * jsonk_encode.c) that just want a pass/fail pass NULL for both.
 *
 * Used by BOTH jsonk_encode() (validating the input PHP value before
 * serializing it) and jsonk_decode() (validating the zval tree
 * jsonk_decode_impl() already built from the simdjson DOM, as a second
 * pass -- see docs/DECISIONS.md "Decode/encode + schema: two passes, not fused
 * (v1)" for why decode doesn't have its own DOM-walking validator). One
 * validator implementation instead of two keeps schema semantics from
 * drifting between encode and decode. */
bool jsonk_schema_validate_zval(
	jsonk_schema_node *schema,
	zval *value,
	smart_str *path,
	zend_long depth_remaining,
	HashTable *evaluated_props,
	HashTable *evaluated_items
);

/* JSON-semantic equality (used for "enum"/"const"): same type category,
 * numbers compared by value regardless of int-vs-float, strings exact,
 * arrays same length and pairwise equal by index, objects/assoc arrays
 * same key set and pairwise equal values regardless of order. */
bool jsonk_values_equal(zval *a, zval *b);

/* Appends "/<segment>" to `path`, RFC 6901-escaping '~' and '/' in
 * `segment`. Callers save `ZSTR_LEN(path->s)` before calling and restore it
 * afterward to "pop" the segment back off (smart_str never reallocates
 * smaller, so this is a cheap length reset, not a real deallocation). */
void jsonk_path_push(smart_str *path, const char *segment, size_t segment_len);
void jsonk_path_push_index(smart_str *path, size_t index);

#ifdef __cplusplus
}
#endif

#endif /* JSONK_SCHEMA_H */
