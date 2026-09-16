#!/bin/bash
set -e
cd "$(dirname "${BASH_SOURCE[0]}")/../.."
rm -rf autom4te.cache build modules .libs Makefile* config.h config.h.in config.log config.nice config.status configure configure.ac libtool run-tests.php *.lo *.la *.dep
phpize
./configure --enable-jsonk
make -j"$(nproc)" 2>&1 | tail -60
