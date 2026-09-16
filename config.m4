dnl config.m4 for extension jsonk

PHP_ARG_ENABLE([jsonk],
  [whether to enable jsonk support],
  [AS_HELP_STRING([--enable-jsonk],
    [Enable jsonk support (fast JSON encode/decode with JSON Schema
     validation, built on simdjson + yyjson). Sources are vendored under
     ./vendor/simdjson and ./vendor/yyjson -- run vendor/build/stage.sh
     first for a native dev build; php-wasm-compiler's own build
     populates the same layout before compiling the WASM target, rather
     than pointing at an external path (see CLAUDE.md's "vendoring"
     decision)])],
  [no])

if test "$PHP_JSONK" != "no"; then
  if test ! -f "vendor/simdjson/simdjson.h"; then
    AC_MSG_ERROR([simdjson not found at vendor/simdjson/simdjson.h -- run vendor/build/stage.sh first])
  fi
  if test ! -f "vendor/yyjson/yyjson.h"; then
    AC_MSG_ERROR([yyjson not found at vendor/yyjson/yyjson.h -- run vendor/build/stage.sh first])
  fi

  AC_DEFINE(HAVE_JSONK, 1, [Whether you have jsonk])

  PHP_ADD_INCLUDE([vendor/simdjson])
  PHP_ADD_INCLUDE([vendor/yyjson])

  dnl simdjson (v4.x) requires C++17. PHP_REQUIRE_CXX() runs AC_PROG_CXX;
  dnl its own PHP_ADD_LIBRARY([stdc++]) call (no target var) does NOT
  dnl reliably reach *this* extension's actual link line -- confirmed by
  dnl a real build: the .so linked clean but failed to *load*
  dnl ("undefined symbol: _ZTVN10__cxxabiv120__si_class_type_infoE", a
  dnl C++ RTTI symbol) because the final link step still ran through the
  dnl plain C linker with no `-lstdc++`. Fixed by explicitly binding
  dnl stdc++ to this extension's own SHARED_LIBADD var, same pattern any
  dnl vendored-library Dockerfile in php-wasm-compiler already uses for
  dnl its own dependencies. See CLAUDE.md's "simdjson bridge" decision for
  dnl why jsonk_decode.cpp is the only first-party C++ file; yyjson (a
  dnl plain C library) needs no such treatment.
  PHP_REQUIRE_CXX()
  PHP_ADD_LIBRARY(stdc++, 1, JSONK_SHARED_LIBADD)
  PHP_SUBST(JSONK_SHARED_LIBADD)
  CXXFLAGS="$CXXFLAGS -std=c++17"

  PHP_NEW_EXTENSION(jsonk,
    jsonk.c jsonk_error.c jsonk_schema.c jsonk_regex.c jsonk_format.c jsonk_fetch.c jsonk_encode.c jsonk_decode.cpp \
    vendor/simdjson/simdjson.cpp vendor/yyjson/yyjson.c,
    $ext_shared)

  PHP_ADD_BUILD_DIR([$ext_builddir/vendor/simdjson], [1])
  PHP_ADD_BUILD_DIR([$ext_builddir/vendor/yyjson], [1])
fi
