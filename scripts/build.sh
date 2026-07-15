#!/usr/bin/env bash
#
# Build pdo_duckdb against the vendored DuckDB C library and run the test suite.
# Requires: phpize, php-config, a C compiler, make, and third_party/duckdb
# populated (run scripts/fetch-duckdb.sh first).
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if [ ! -r third_party/duckdb/duckdb.h ]; then
  echo "third_party/duckdb is empty — running scripts/fetch-duckdb.sh" >&2
  ./scripts/fetch-duckdb.sh
fi

# Clean any previous out-of-tree build state.
[ -f Makefile ] && make clean >/dev/null 2>&1 || true
phpize --clean >/dev/null 2>&1 || true

phpize
./configure --with-pdo-duckdb
make

export LD_LIBRARY_PATH="$ROOT/third_party/duckdb${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

echo
echo "== module loads =="
php -d extension="$ROOT/modules/pdo_duckdb.so" -m | grep -i pdo

echo
echo "== test suite =="
# NB: -d options must come *after* run-tests.php so they are forwarded to the
# child processes that actually run each test.
NO_INTERACTION=1 REPORT_EXIT_STATUS=1 \
  php run-tests.php -q --show-diff -d extension="$ROOT/modules/pdo_duckdb.so" tests
