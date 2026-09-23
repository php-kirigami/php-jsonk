# Instructions

## Building natively

With PHP development headers available:

```bash
bash vendor/build/stage.sh      # downloads simdjson + yyjson (matrix.json-pinned)
phpize
./configure --enable-jsonk
make
php -n -d extension=modules/jsonk.so your-test.php
```

This machine's WSL Ubuntu has no PHP or compiler installed. Two ways
around it:

- **Docker**: a throwaway `php:8.x-cli` container with `build-essential`
  (decision 17). Docker is often busy with `php-wasm-compiler` builds.
- **No-install toolchain in WSL** (decision 22): `apt-get download` +
  `dpkg -x` into `/tmp` for `php8.5-dev` (headers), `php8.5-cli` (+
  `libargon2-1`), `libpcre2-dev`, and gcc/g++ 15 with binutils,
  `libstdc++-15-dev`, `libc6-dev`. Point gcc at the extracted tree with
  `-B` and `--sysroot`, copy the system `libc.so.6`/`libm.so.6`/
  `libmvec.so.1`/`libgcc_s.so.1`/`libstdc++.so.6`/`ld-linux-x86-64.so.2`
  into the sysroot for the final link, and compile the sources listed in
  `config.m4` directly (C files with gcc, `jsonk_decode.cpp` and
  `simdjson.cpp` with `g++ -std=c++17`, then `g++ -shared`). Run the
  extracted `php8.5` with `LD_LIBRARY_PATH` pointing at its libraries.

## Releasing

1. Bump `PHP_JSONK_VERSION` in `php_jsonk.h`.
2. Commit, then an annotated tag `vX.Y.Z` (`git tag -a vX.Y.Z -m "…"`),
   and push `main` with the tag.
3. `php-wasm-compiler` downloads the tag's GitHub archive
   (`matrix.json` → `jsonk.versions`); add the new tag there before the
   next PHP-WASM build.
