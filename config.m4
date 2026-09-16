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

  dnl Under Emscripten, simdjson.h's own architecture detection sees
  dnl whatever __x86_64__-style macro the *consuming build* forces (e.g.
  dnl php-wasm-compiler's pipeline-wide -D__x86_64__, used purely to make
  dnl zend_long 64-bit -- see php-wasm-compiler's CLAUDE.md "64 bit long
  dnl support" section) and, since SIMDJSON_IMPLEMENTATION_HASWELL/
  dnl WESTMERE default to "on" whenever SIMDJSON_IS_X86_64 is true,
  dnl compiles those x86-specific backends' source for potential runtime
  dnl dispatch -- pulling in <x86intrin.h>, which needs real x86 hardware
  dnl builtins (CPUID/RDTSC/AMD-only SSE4a extrq/insertq, etc.) that have
  dnl no wasm equivalent and cannot be made available under Emscripten by
  dnl any combination of -msse*/-msimd128 flags (verified directly: even
  dnl with -msimd128 -msse4.2, <x86intrin.h>'s own ia32intrin.h/ammintrin.h
  dnl sub-includes still hard-error on undeclared __builtin_ia32_* and "This
  dnl header is only meant to be used on x86 and x64 architecture"). The
  dnl actual fix is to stop those backends from being compiled at all:
  dnl SIMDJSON_IMPLEMENTATION_ICELAKE/HASWELL/WESTMERE=0 forces
  dnl SIMDJSON_BUILTIN_IMPLEMENTATION to resolve to "fallback" (a portable,
  dnl non-SIMD implementation) on its own, since simdjson.h's own logic
  dnl only enables fallback once none of the SIMD-specific ones "can always
  dnl run". Found via a real php-wasm-compiler build; -msimd128 alone
  dnl (an earlier attempt) was not sufficient, only pushed the same class
  dnl of error further down the same unconditionally-compiled header
  dnl chain. These defines are meaningless (but harmless) on a native
  dnl build; scoped to Emscripten anyway for clarity.
  dnl simdjson.h separately (not just simdjson.cpp) also unconditionally
  dnl #include's <emmintrin.h> once SIMDJSON_EXPERIMENTAL_HAS_SSE2 resolves
  dnl true (its own experimental/portable-DOM-API code path) -- properly
  dnl `#ifndef`-guarded, unlike SIMDJSON_IS_X86_64 itself (see below), so
  dnl overridable via -D here directly, no patch needed for this one.
  case $CXX in
    *em++*) CXXFLAGS="$CXXFLAGS -DSIMDJSON_IMPLEMENTATION_ICELAKE=0 -DSIMDJSON_IMPLEMENTATION_HASWELL=0 -DSIMDJSON_IMPLEMENTATION_WESTMERE=0 -DSIMDJSON_EXPERIMENTAL_HAS_SSE2=0" ;;
  esac

  dnl Even with all three x86 SIMD implementations disabled above,
  dnl simdjson.cpp's own runtime-dispatch CPU-feature detection
  dnl (detect_supported_architectures()) is gated by a **raw**
  dnl `#elif defined(__x86_64__) || defined(_M_AMD64)` -- not any
  dnl SIMDJSON_* indirection, so it can't be overridden via -D (that
  dnl branch's own body does an unguarded `#define SIMDJSON_IS_X86_64 1`
  dnl that would silently re-win over a command-line override anyway,
  dnl confirmed by testing). That branch emits real x86 `cpuid`/`xgetbv`
  dnl inline asm, which has no wasm equivalent and fails to compile
  dnl under Emscripten regardless of any flag. php-wasm-compiler
  dnl (which downloads simdjson.cpp directly, independent of this
  dnl repo's own vendor/build/stage.sh) carries a small patch
  dnl (patches/simdjson/emscripten-skip-x86-cpuid-detection.patch) adding
  dnl `&& !defined(__EMSCRIPTEN__)` to that one condition, falling through
  dnl to the existing portable `instruction_set::DEFAULT` branch instead --
  dnl see that repo's own CLAUDE.md for the full writeup. Not something
  dnl this repo's own config.m4 can fix on its own, since it doesn't own
  dnl simdjson.cpp's source.

  PHP_NEW_EXTENSION(jsonk,
    jsonk.c jsonk_error.c jsonk_schema.c jsonk_regex.c jsonk_format.c jsonk_fetch.c jsonk_encode.c jsonk_decode.cpp \
    vendor/simdjson/simdjson.cpp vendor/yyjson/yyjson.c,
    $ext_shared)

  PHP_ADD_BUILD_DIR([$ext_builddir/vendor/simdjson], [1])
  PHP_ADD_BUILD_DIR([$ext_builddir/vendor/yyjson], [1])
fi
