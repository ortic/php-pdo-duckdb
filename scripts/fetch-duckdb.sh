#!/usr/bin/env bash
#
# Download and unpack the pinned DuckDB C library into third_party/duckdb.
# The prebuilt binaries are intentionally not committed to the repo.
#
# Usage: scripts/fetch-duckdb.sh [version]
#
set -euo pipefail

DUCKDB_VERSION="${1:-v1.5.4}"
DEST="$(cd "$(dirname "$0")/.." && pwd)/third_party/duckdb"

# Map uname to DuckDB release asset names.
os="$(uname -s)"
arch="$(uname -m)"
case "$os" in
  Linux)  plat="linux" ;;
  Darwin) plat="osx" ;;
  *) echo "Unsupported OS: $os" >&2; exit 1 ;;
esac
case "$arch" in
  x86_64|amd64) a="amd64" ;;
  arm64|aarch64) a="arm64" ;;
  *) echo "Unsupported arch: $arch" >&2; exit 1 ;;
esac

if [ "$plat" = "osx" ]; then
  asset="libduckdb-osx-universal.zip"
else
  asset="libduckdb-${plat}-${a}.zip"
fi

url="https://github.com/duckdb/duckdb/releases/download/${DUCKDB_VERSION}/${asset}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "Fetching $url"
curl -fsSL -o "$tmp/libduckdb.zip" "$url"
mkdir -p "$DEST"
unzip -o "$tmp/libduckdb.zip" -d "$DEST"

echo "DuckDB $DUCKDB_VERSION unpacked into $DEST:"
ls -1 "$DEST"
