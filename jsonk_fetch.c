#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_jsonk.h"
#include "jsonk_error.h"
#include "jsonk_fetch.h"
#include "zend_modules.h"
#include "zend_constants.h"
#include <time.h>
#include <string.h>

/* How long a fetched external schema document is trusted before
 * re-fetching. Deliberately short (real-world external $ref targets are
 * meant to be stable, versioned documents) but non-zero, so a burst of
 * validate calls against the same schema within one request/short window
 * doesn't hit the network repeatedly. */
#define JSONK_FETCH_CACHE_TTL 300

typedef struct {
	zend_string *body; /* persistent-allocated -- see jsonk_fetch_cache_init() */
	time_t fetched_at;
} jsonk_fetch_cache_entry;

/* Process-lifetime (not request-scoped) cache: initialized once in
 * PHP_MINIT, destroyed once in PHP_MSHUTDOWN. In a non-ZTS SAPI (the
 * default for PHP-FPM and the CLI) this is a plain process-wide C global
 * and correctly persists across requests handled by the same worker. Not
 * thread-safe: no lock guards access, so a ZTS build (a threaded SAPI)
 * could race on it. Accepted for v1 -- see CLAUDE.md. */
static HashTable jsonk_fetch_cache;
static bool jsonk_fetch_cache_ready = false;

static void jsonk_fetch_cache_entry_dtor(zval *zv)
{
	jsonk_fetch_cache_entry *entry = (jsonk_fetch_cache_entry *) Z_PTR_P(zv);
	zend_string_release(entry->body);
	pefree(entry, 1);
}

void jsonk_fetch_cache_init(void)
{
	zend_hash_init(&jsonk_fetch_cache, 8, NULL, jsonk_fetch_cache_entry_dtor, 1);
	jsonk_fetch_cache_ready = true;
}

void jsonk_fetch_cache_shutdown(void)
{
	if (jsonk_fetch_cache_ready) {
		zend_hash_destroy(&jsonk_fetch_cache);
		jsonk_fetch_cache_ready = false;
	}
}

bool jsonk_curl_available(void)
{
	return zend_hash_str_exists(&module_registry, ZEND_STRL("curl")) != 0;
}

static bool call_php_func(const char *name, zval *retval, uint32_t argc, zval *argv)
{
	zval func_name;
	zend_result rc;

	ZVAL_STRING(&func_name, name);
	rc = call_user_function(NULL, NULL, &func_name, retval, argc, argv);
	zval_ptr_dtor(&func_name);

	return rc == SUCCESS;
}

/* Prefix for every key this extension stores in APCu, to avoid colliding
 * with anything else sharing the same cache. */
#define JSONK_APCU_KEY_PREFIX "jsonk:fetch:"

/* APCu (when loaded AND actually active -- see apcu_enabled() below) is
 * preferred over jsonk's own process-global HashTable cache: real shared
 * memory with its own TTL handling, and -- unlike the hand-rolled
 * fallback -- safe under a threaded/ZTS SAPI (see CLAUDE.md). ext/curl's
 * module-dependency treatment applies here too: this is a nice-to-have,
 * not required, so no zend_module_dep entry -- just a runtime check.
 * apcu_enabled() specifically (not just "is the module loaded") matters
 * because APCu commonly ships loaded-but-disabled for the CLI SAPI unless
 * `apc.enable_cli=1` is set. */
bool jsonk_apcu_available(void)
{
	zval ret;
	bool available;

	if (!zend_hash_str_exists(&module_registry, ZEND_STRL("apcu"))) {
		return false;
	}

	ZVAL_UNDEF(&ret);
	available = call_php_func("apcu_enabled", &ret, 0, NULL) && Z_TYPE(ret) == IS_TRUE;
	if (Z_TYPE(ret) != IS_UNDEF) zval_ptr_dtor(&ret);

	return available;
}

static zend_string *apcu_try_fetch(const char *url, size_t url_len)
{
	zend_string *key = strpprintf(0, JSONK_APCU_KEY_PREFIX "%.*s", (int) url_len, url);
	zval key_zv, ret;
	zend_string *result = NULL;

	ZVAL_STR(&key_zv, key);
	ZVAL_UNDEF(&ret);

	if (call_php_func("apcu_fetch", &ret, 1, &key_zv) && Z_TYPE(ret) == IS_STRING) {
		result = zend_string_init(Z_STRVAL(ret), Z_STRLEN(ret), 0);
	}

	if (Z_TYPE(ret) != IS_UNDEF) zval_ptr_dtor(&ret);
	zval_ptr_dtor(&key_zv);
	return result;
}

static void apcu_try_store(const char *url, size_t url_len, zend_string *body)
{
	zend_string *key = strpprintf(0, JSONK_APCU_KEY_PREFIX "%.*s", (int) url_len, url);
	zval args[3], ret;

	ZVAL_STR(&args[0], key);
	ZVAL_STR_COPY(&args[1], body); /* `body` itself stays owned by the caller, used afterward independently */
	ZVAL_LONG(&args[2], JSONK_FETCH_CACHE_TTL);
	ZVAL_UNDEF(&ret);

	call_php_func("apcu_store", &ret, 3, args);
	if (Z_TYPE(ret) != IS_UNDEF) zval_ptr_dtor(&ret);

	zval_ptr_dtor(&args[0]);
	zval_ptr_dtor(&args[1]);
}

/* Resolves a CURLOPT_ / CURLINFO_ constant name to its runtime value via PHP's own
 * registered constant, rather than hardcoding libcurl's numeric enum --
 * those values are effectively ABI-stable in practice, but resolving them
 * this way needs no verification against a vendored curl.h and can't
 * silently drift. Returns false (leaving the option unset) if the
 * constant isn't defined for some reason -- never guesses. */
static bool curl_setopt_long(zval *ch, const char *opt_const_name, zend_long value)
{
	zval *opt_const = zend_get_constant_str(opt_const_name, strlen(opt_const_name));
	zval args[3], ret;
	bool ok;

	if (!opt_const) return false;

	ZVAL_COPY_VALUE(&args[0], ch);
	ZVAL_LONG(&args[1], Z_LVAL_P(opt_const));
	ZVAL_LONG(&args[2], value);

	ok = call_php_func("curl_setopt", &ret, 3, args);
	if (ok) zval_ptr_dtor(&ret);
	return ok;
}

static bool curl_setopt_str(zval *ch, const char *opt_const_name, const char *value)
{
	zval *opt_const = zend_get_constant_str(opt_const_name, strlen(opt_const_name));
	zval args[3], ret;
	bool ok;

	if (!opt_const) return false;

	ZVAL_COPY_VALUE(&args[0], ch);
	ZVAL_LONG(&args[1], Z_LVAL_P(opt_const));
	ZVAL_STRING(&args[2], value);

	ok = call_php_func("curl_setopt", &ret, 3, args);
	zval_ptr_dtor(&args[2]);
	if (ok) zval_ptr_dtor(&ret);
	return ok;
}

static zend_long curl_getinfo_long(zval *ch, const char *info_const_name)
{
	zval *info_const = zend_get_constant_str(info_const_name, strlen(info_const_name));
	zval args[2], ret;
	zend_long result = 0;

	if (!info_const) return 0;

	ZVAL_COPY_VALUE(&args[0], ch);
	ZVAL_LONG(&args[1], Z_LVAL_P(info_const));

	if (call_php_func("curl_getinfo", &ret, 2, args)) {
		if (Z_TYPE(ret) == IS_LONG) result = Z_LVAL(ret);
		zval_ptr_dtor(&ret);
	}
	return result;
}

static zend_string *do_fetch(const char *url, size_t url_len)
{
	zval ch, ret;
	zend_string *result = NULL;
	zend_long http_code;
	char url_cstr_buf[2048];
	const char *url_cstr;
	zend_string *url_zstr = NULL;

	/* Defensive: Zend's calling convention only guarantees these are set
	 * to the real return value when the call actually succeeds. Every
	 * check below reads Z_TYPE() unconditionally (success or not), so
	 * both must be well-defined (IS_UNDEF at worst) even on failure. */
	ZVAL_UNDEF(&ch);
	ZVAL_UNDEF(&ret);

	/* curl_setopt(CURLOPT_URL, ...) needs a NUL-terminated C string;
	 * `url` here is a substring (the "$ref" text minus any "#fragment"),
	 * so it isn't guaranteed to already be NUL-terminated at url_len. */
	if (url_len < sizeof(url_cstr_buf)) {
		memcpy(url_cstr_buf, url, url_len);
		url_cstr_buf[url_len] = '\0';
		url_cstr = url_cstr_buf;
	} else {
		url_zstr = zend_string_init(url, url_len, 0);
		url_cstr = ZSTR_VAL(url_zstr);
	}

	if (!call_php_func("curl_init", &ch, 0, NULL) || Z_TYPE(ch) != IS_OBJECT) {
		jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "curl_init() failed while resolving an external \"$ref\"");
		if (Z_TYPE(ch) != IS_UNDEF) zval_ptr_dtor(&ch);
		if (url_zstr) zend_string_release(url_zstr);
		return NULL;
	}

	curl_setopt_str(&ch, "CURLOPT_URL", url_cstr);
	curl_setopt_long(&ch, "CURLOPT_RETURNTRANSFER", 1);
	curl_setopt_long(&ch, "CURLOPT_FOLLOWLOCATION", 1);
	curl_setopt_long(&ch, "CURLOPT_MAXREDIRS", 5);
	curl_setopt_long(&ch, "CURLOPT_TIMEOUT", 10);
	curl_setopt_long(&ch, "CURLOPT_CONNECTTIMEOUT", 5);
	curl_setopt_long(&ch, "CURLOPT_SSL_VERIFYPEER", 1);
	curl_setopt_long(&ch, "CURLOPT_SSL_VERIFYHOST", 2);
	curl_setopt_str(&ch, "CURLOPT_USERAGENT", "php-jsonk/" PHP_JSONK_VERSION);
	{
		/* Hard-restrict to http(s) -- the caller already checked the
		 * scheme before calling jsonk_fetch_url(), but this closes the
		 * door on curl itself ever following a redirect (CURLOPT_
		 * FOLLOWLOCATION, above) into a non-http(s) scheme such as
		 * file:// -- a real local-file-read risk otherwise, since
		 * external "$ref" is active by default (see CLAUDE.md). */
		zval *http_proto = zend_get_constant_str(ZEND_STRL("CURLPROTO_HTTP"));
		zval *https_proto = zend_get_constant_str(ZEND_STRL("CURLPROTO_HTTPS"));
		if (http_proto && https_proto) {
			zend_long mask = Z_LVAL_P(http_proto) | Z_LVAL_P(https_proto);
			curl_setopt_long(&ch, "CURLOPT_PROTOCOLS", mask);
			curl_setopt_long(&ch, "CURLOPT_REDIR_PROTOCOLS", mask);
		}
	}

	if (!call_php_func("curl_exec", &ret, 1, &ch) || Z_TYPE(ret) != IS_STRING) {
		jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "Failed to fetch external \"$ref\" URL \"%s\"", url_cstr);
		if (Z_TYPE(ret) != IS_UNDEF) zval_ptr_dtor(&ret);
		goto cleanup;
	}

	http_code = curl_getinfo_long(&ch, "CURLINFO_HTTP_CODE");
	if (http_code < 200 || http_code >= 300) {
		jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "External \"$ref\" URL \"%s\" returned HTTP %ld", url_cstr, (long) http_code);
		zval_ptr_dtor(&ret);
		goto cleanup;
	}

	result = zend_string_init(Z_STRVAL(ret), Z_STRLEN(ret), 0);

	/* Prefer APCu when it's actually active; fall back to jsonk's own
	 * process-global cache otherwise (see jsonk_apcu_available()). */
	if (jsonk_apcu_available()) {
		apcu_try_store(url, url_len, result);
	} else {
		jsonk_fetch_cache_entry *entry = pemalloc(sizeof(jsonk_fetch_cache_entry), 1);
		entry->body = zend_string_init(Z_STRVAL(ret), Z_STRLEN(ret), 1);
		entry->fetched_at = time(NULL);
		zend_hash_str_update_ptr(&jsonk_fetch_cache, url, url_len, entry);
	}

	zval_ptr_dtor(&ret);

cleanup: {
		zval close_ret;
		if (call_php_func("curl_close", &close_ret, 1, &ch)) {
			zval_ptr_dtor(&close_ret);
		}
	}
	zval_ptr_dtor(&ch);
	if (url_zstr) zend_string_release(url_zstr);
	return result;
}

zend_string *jsonk_fetch_url(const char *url, size_t url_len)
{
	jsonk_fetch_cache_entry *cached;

	if (!jsonk_curl_available()) {
		jsonk_set_error(JSONK_ERROR_SCHEMA_INVALID, "External \"$ref\" resolution requires ext/curl, which is not loaded");
		return NULL;
	}

	if (jsonk_apcu_available()) {
		zend_string *from_apcu = apcu_try_fetch(url, url_len);
		if (from_apcu) {
			return from_apcu;
		}
	} else {
		cached = zend_hash_str_find_ptr(&jsonk_fetch_cache, url, url_len);
		if (cached && (time(NULL) - cached->fetched_at) < JSONK_FETCH_CACHE_TTL) {
			return zend_string_init(ZSTR_VAL(cached->body), ZSTR_LEN(cached->body), 0);
		}
	}

	return do_fetch(url, url_len);
}
