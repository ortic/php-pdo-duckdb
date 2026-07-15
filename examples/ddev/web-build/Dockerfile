# Build the pdo_duckdb extension into the DDEV web container.
#
# Copy this file to `.ddev/web-build/Dockerfile` in your project, then run
# `ddev restart`. It fetches the extension source and builds it for every PHP
# version the web image ships, so `ddev config --php-version=X.Y` keeps working.
#
# Pin the DuckDB release with:  ddev config --web-environment-add=DUCKDB_VERSION=v1.5.4
ARG BASE_IMAGE
FROM ${BASE_IMAGE}

ARG DUCKDB_VERSION=v1.5.4

RUN set -eux; \
    curl -fsSL "https://github.com/ortic/php-pdo-duckdb/archive/refs/heads/main.tar.gz" | tar xz -C /tmp; \
    cd /tmp/php-pdo-duckdb-main; \
    for cfg in /usr/bin/php-config[0-9].[0-9]; do \
        ver="$(basename "$cfg" | sed 's/php-config//')"; \
        pz="/usr/bin/phpize${ver}"; \
        [ -x "$pz" ] || continue; \
        echo ">>> building pdo_duckdb for PHP ${ver}"; \
        PHPIZE="$pz" PHP_CONFIG="$cfg" DUCKDB_VERSION="${DUCKDB_VERSION}" ./scripts/install.sh; \
    done; \
    rm -rf /tmp/php-pdo-duckdb-main
