#ifndef JSONK_FETCH_H
#define JSONK_FETCH_H

#include "php_jsonk.h"

#ifdef __cplusplus
extern "C" {
#endif

/* External "$ref" resolution (an http(s) URL, as opposed to a
 * same-document "#/..." fragment) via PHP's own ext/curl, called through
 * userland functions (curl_init/curl_setopt/curl_exec/curl_getinfo) --
 * not by linking libcurl directly. See docs/DECISIONS.md for why: it reuses
 * whatever curl configuration (proxy, CA bundle, etc.) the surrounding
 * PHP install already has, and needs zero new build-time dependency.
 *
 * Active by default when a "$ref" turns out to be an external URL (no
 * opt-in flag -- a deliberate choice, see docs/DECISIONS.md's SSRF discussion).
 * ext/curl itself is only an OPTIONAL module dependency (unlike the hard
 * "json"/"pcre" ones): most of jsonk works fine without it, an external
 * "$ref" just fails with a clear error if it's absent.
 */

/* Fetches `url`/`url_len` (must already be known http(s) -- the caller
 * checks the scheme before calling this). Cached (TTL-based, see
 * JSONK_FETCH_CACHE_TTL in jsonk_fetch.c) so the same schema compiled
 * repeatedly doesn't refetch every time -- no compiled-schema cache
 * exists yet (see docs/TODO.md), so this is the one layer of
 * caching external "$ref" currently gets. Prefers APCu when it's loaded
 * AND active (apcu_enabled()), since it's real shared memory with its own
 * TTL handling and is safe under a threaded/ZTS SAPI; falls back to
 * jsonk's own process-global, non-thread-safe cache otherwise.
 *
 * Returns an owned, request-scoped (non-persistent) zend_string with the
 * response body on a 2xx response, or NULL on any failure (ext/curl not
 * loaded, network error, non-2xx status) -- jsonk_set_error() is already
 * called before returning NULL. */
zend_string *jsonk_fetch_url(const char *url, size_t url_len);

/* True if ext/curl is loaded. */
bool jsonk_curl_available(void);

/* True if APCu is loaded AND actually active (apcu_enabled()) -- see the
 * comment above jsonk_fetch_url() and its own doc comment in jsonk_fetch.c
 * for why "loaded" alone isn't enough (APCu commonly ships disabled for
 * the CLI SAPI). Exposed (not just used internally) so PHP_MINFO_FUNCTION
 * can report which cache backend jsonk_fetch_url() is actually using. */
bool jsonk_apcu_available(void);

/* Process-lifetime FALLBACK cache lifecycle (used only when APCu isn't
 * available -- see jsonk_fetch_url()) -- called from
 * PHP_MINIT/PHP_MSHUTDOWN in jsonk.c, NOT per-request (RINIT/RSHUTDOWN):
 * the cache is deliberately meant to survive across requests in a
 * persistent worker (traditional non-ZTS PHP-FPM/CLI). See docs/DECISIONS.md for
 * the ZTS caveat -- this cache has no locking and assumes a non-threaded
 * SAPI (APCu itself has none of these caveats, which is why it's
 * preferred whenever it's actually usable). */
void jsonk_fetch_cache_init(void);
void jsonk_fetch_cache_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* JSONK_FETCH_H */
