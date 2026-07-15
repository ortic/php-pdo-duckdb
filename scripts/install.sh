#!/usr/bin/env bash
#
# Build and install the pdo_duckdb extension into a PHP runtime, and enable it.
# Designed to run as root inside an image build (Docker / DDEV web container),
# but works anywhere the build tools (phpize, a C compiler, make) are present.
#
# Environment overrides:
#   DUCKDB_VERSION   DuckDB release to link against       (default: v1.5.4)
#   PHPIZE           phpize binary                        (default: phpize)
#   PHP_CONFIG       php-config binary                    (default: php-config)
#   DUCKDB_PREFIX    where to install the DuckDB C library (default: /usr/local)
#
# For a DDEV/Docker image that ships several PHP versions, call this once per
# version, e.g.  PHPIZE=phpize8.3 PHP_CONFIG=php-config8.3 scripts/install.sh
#
set -euo pipefail

DUCKDB_VERSION="${DUCKDB_VERSION:-v1.5.4}"
PHPIZE="${PHPIZE:-phpize}"
PHP_CONFIG="${PHP_CONFIG:-php-config}"
DUCKDB_PREFIX="${DUCKDB_PREFIX:-/usr/local}"

SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"

case "$(uname -m)" in
  x86_64|amd64)  asset="libduckdb-linux-amd64.zip" ;;
  aarch64|arm64) asset="libduckdb-linux-arm64.zip" ;;
  *) echo "pdo_duckdb: unsupported architecture $(uname -m)" >&2; exit 1 ;;
esac

# 1. Install the DuckDB C library (header + shared object), once.
if [ ! -e "${DUCKDB_PREFIX}/lib/libduckdb.so" ]; then
  echo ">> fetching libduckdb ${DUCKDB_VERSION} (${asset})"
  tmp="$(mktemp -d)"
  curl -fsSL -o "${tmp}/libduckdb.zip" \
    "https://github.com/duckdb/duckdb/releases/download/${DUCKDB_VERSION}/${asset}"
  unzip -oq "${tmp}/libduckdb.zip" -d "${tmp}"
  install -Dm644 "${tmp}/duckdb.h"     "${DUCKDB_PREFIX}/include/duckdb.h"
  install -Dm755 "${tmp}/libduckdb.so" "${DUCKDB_PREFIX}/lib/libduckdb.so"
  ldconfig 2>/dev/null || true
  rm -rf "${tmp}"
fi

# 2. Build the extension against the selected PHP (in a throwaway copy so the
#    source tree stays clean and repeated per-version builds don't collide).
builddir="$(mktemp -d)"
cp -a "${SRC_DIR}/." "${builddir}/"
cd "${builddir}"

# Strip anything left over from a previous or host build so phpize starts from
# clean source (stale autotools files bake in absolute paths that break here).
rm -rf .git third_party modules autom4te.cache build .deps .libs
rm -f Makefile Makefile.fragments Makefile.global Makefile.objects config.status \
      config.nice config.log config.h config.h.in configure configure.ac \
      aclocal.m4 acinclude.m4 libtool run-tests.php ./*.lo ./*.la ./*.loT ./*.dep

"${PHPIZE}"
./configure --with-pdo-duckdb="${DUCKDB_PREFIX}" --with-php-config="${PHP_CONFIG}"
make -j"$(nproc)"
make install

# 3. Enable it for that PHP by dropping an ini into its scan dir. Load order
#    vs. pdo does not matter: the module declares pdo as a required dependency.
php_bin="$("${PHP_CONFIG}" --php-binary 2>/dev/null || echo php)"
scandir="$("${php_bin}" -r 'echo PHP_CONFIG_FILE_SCAN_DIR;' 2>/dev/null || true)"
if [ -n "${scandir}" ]; then
  mkdir -p "${scandir}"
  echo "extension=pdo_duckdb.so" > "${scandir}/pdo_duckdb.ini"
  echo ">> enabled via ${scandir}/pdo_duckdb.ini"
else
  echo ">> could not determine the ini scan dir; add 'extension=pdo_duckdb.so' to php.ini manually" >&2
fi

cd /
rm -rf "${builddir}"
echo ">> pdo_duckdb installed"
