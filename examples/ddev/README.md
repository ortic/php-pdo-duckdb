# Using pdo_duckdb with DDEV

DDEV's web container doesn't ship `pdo_duckdb`, so it has to be built into the
image. This example does that with a custom web-build Dockerfile.

## Setup

1. Copy the Dockerfile into your project:

   ```bash
   mkdir -p .ddev/web-build
   cp path/to/examples/ddev/web-build/Dockerfile .ddev/web-build/Dockerfile
   ```

2. (Optional) pin a DuckDB version:

   ```bash
   ddev config --web-environment-add=DUCKDB_VERSION=v1.5.4
   ```

3. Rebuild:

   ```bash
   ddev restart
   ```

4. Verify:

   ```bash
   ddev exec php -m | grep pdo_duckdb
   ddev exec php -r 'new PDO("duckdb::memory:"); echo "ok\n";'
   ```

The first build compiles the extension for each PHP version in the image, so it
takes a few minutes; afterwards it's cached until you change the Dockerfile.

## Then install the Laravel driver

Once the extension is present:

```bash
ddev composer require ortic/laravel-duckdb-pdo
```

and add a `duckdb` connection to `config/database.php` (see that package's
README).

## CI

The same `scripts/install.sh` works in a CI image. For a plain PHP container:

```dockerfile
RUN apt-get update && apt-get install -y $PHPIZE_DEPS unzip curl \
 && curl -fsSL https://github.com/ortic/php-pdo-duckdb/archive/refs/heads/main.tar.gz | tar xz -C /tmp \
 && /tmp/php-pdo-duckdb-main/scripts/install.sh
```
