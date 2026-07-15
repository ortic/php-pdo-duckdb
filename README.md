# pdo_duckdb

A **native** PHP PDO driver for [DuckDB](https://duckdb.org), written in C.

Unlike the FFI-based [satur-io/duckdb-php](https://github.com/satur-io/duckdb-php),
this is a real PDO driver, so DuckDB plugs into the standard `PDO` / `PDOStatement`
API and any PDO-based framework — most notably Laravel's `Illuminate\Database`,
which makes an in-memory DuckDB attractive for fast, isolated tests.

> **Status: early development.** See [`PLAN.md`](PLAN.md) for the full roadmap.
> **Phase 4 (current):** connect, execute, fetch, parameter binding,
> transactions, SQLSTATE-mapped errors, `lastInsertId`, plus **full result
> type coverage** and `getColumnMeta()`.
>
> **Result type coverage:** BOOLEAN; all integer widths (HUGEINT / UHUGEINT /
> large UBIGINT as exact strings); FLOAT / DOUBLE; DECIMAL (exact string);
> VARCHAR; BLOB; SQL NULL; DATE / TIME / TIMESTAMP (all precisions & TZ) /
> TIME_TZ / INTERVAL and UUID as strings; ENUM as its label; and LIST / STRUCT
> / MAP / ARRAY as JSON strings. `PDO::ATTR_STRINGIFY_FETCHES` is honored.

## Requirements

- PHP 8.1–8.4 with development headers (`phpize`, `php-config`) and the PDO core.
- A C compiler and `make`.
- The DuckDB C library (`duckdb.h` + `libduckdb.so`). A pinned copy can be
  fetched into `third_party/duckdb` with the helper script below.

On Ubuntu 24.04 (PHP 8.3):

```bash
sudo apt-get update
sudo apt-get install -y php-cli php-dev php-pdo build-essential unzip
```

## Building

```bash
# 1. Fetch the pinned DuckDB C library into third_party/duckdb/
./scripts/fetch-duckdb.sh            # or: ./scripts/fetch-duckdb.sh v1.5.4

# 2. Build the extension
phpize
./configure --with-pdo-duckdb        # auto-detects third_party/duckdb, then /usr/local, /usr
make

# 3. Try it without installing (libduckdb.so lives in third_party/duckdb)
LD_LIBRARY_PATH="$PWD/third_party/duckdb" \
  php -d extension="$PWD/modules/pdo_duckdb.so" -m | grep -i duckdb
```

To build against a DuckDB library installed elsewhere:

```bash
./configure --with-pdo-duckdb=/opt/duckdb
```

To install system-wide (`make install` puts `pdo_duckdb.so` in the PHP
extension dir; add `extension=pdo_duckdb` to your `php.ini`). Ensure
`libduckdb.so` is on the loader path (e.g. copy it into `/usr/local/lib` and run
`sudo ldconfig`, or set `LD_LIBRARY_PATH`).

## Usage

```php
// In-memory database
$db = new PDO('duckdb::memory:');

// File-backed database
$db = new PDO('duckdb:/path/to/analytics.duckdb');

// With DSN options
$db = new PDO('duckdb:dbname=/path/to/db.duckdb;access_mode=READ_ONLY;threads=4');

$db->getAttribute(PDO::ATTR_DRIVER_NAME);     // "duckdb"
$db->getAttribute(PDO::ATTR_CLIENT_VERSION);  // e.g. "v1.5.4"

// DDL / DML — exec() returns the affected-row count
$db->exec('CREATE TABLE t (id INTEGER, name VARCHAR)');
$db->exec("INSERT INTO t VALUES (1, 'a'), (2, 'b')");   // 2

// Queries
foreach ($db->query('SELECT id, name FROM t ORDER BY id') as $row) {
    echo $row['id'], ' => ', $row['name'], "\n";
}

$count = $db->query('SELECT count(*) FROM t')->fetchColumn();

// Prepared statements (parameter binding arrives in phase 2)
$stmt = $db->prepare('SELECT * FROM t');
$stmt->execute();
$rows = $stmt->fetchAll(PDO::FETCH_ASSOC);

$db->beginTransaction();
$db->commit();
```

### DSN

```
duckdb:<path>
duckdb::memory:
duckdb:dbname=<path>;access_mode=READ_ONLY;read_only=1;threads=<n>;max_memory=<size>
```

- `<path>` — path to a `.duckdb` file, or `:memory:` for an in-memory database.
- `access_mode` — `READ_ONLY` or `READ_WRITE` (DuckDB config option).
- `read_only=1` — convenience alias for `access_mode=READ_ONLY`.
- `threads`, `max_memory` — passed straight to DuckDB configuration.

### Parameter binding

Both positional `?` and named `:name` placeholders work. Native prepared
statements are used by default; set `PDO::ATTR_EMULATE_PREPARES => true` to
substitute values client-side instead (which also lets you reuse the same
named parameter more than once).

Two things to know, because DuckDB is stricter about types than most engines
and PDO stringifies untyped values:

- **Booleans:** binding a raw PHP `bool` through an untyped `execute([$flag])`
  array sends an empty string for `false` (a PDO quirk), which DuckDB will not
  cast to `BOOLEAN`. Bind booleans with `PDO::PARAM_BOOL`, or as integers
  `0`/`1`. (Laravel does this conversion for you.)
- **Arithmetic on bare parameters** such as `SELECT ? + ?` fails unless the
  parameters are typed (`PDO::PARAM_INT`) or cast in SQL (`?::INT + ?::INT`),
  because DuckDB cannot infer a type with no column context. Parameters used
  against columns (`WHERE id = ?`, `INSERT ... VALUES (?)`, `LIMIT ?`) infer
  their type and need no help.

## Using with Laravel

A companion package wires this extension into Laravel's database layer
(connection, query/schema grammars, Eloquent):

- **[ortic/laravel-duckdb](https://github.com/ortic/laravel-duckdb)** — register
  a `duckdb` connection and use migrations, the query builder, transactions, and
  Eloquent as usual. In-memory DuckDB makes a fast, isolated test database.

## Running the tests

```bash
LD_LIBRARY_PATH="$PWD/third_party/duckdb" \
  make test TESTS=tests
```

## Layout

| File | Purpose |
|------|---------|
| `pdo_duckdb.c` | Module entry; registers the driver with the PDO core. |
| `duckdb_driver.c` | Database-handle methods (connect, attrs, quoter, transactions, errors). |
| `duckdb_statement.c` | Statement methods: execute + columnar→row fetch + type decoding. |
| `php_pdo_duckdb_int.h` | Internal shared structs and the error helper. |
| `config.m4` / `config.w32` | Build configuration. |
| `scripts/fetch-duckdb.sh` | Downloads the pinned DuckDB C library. |
| `tests/*.phpt` | PHP test suite. |
| `PLAN.md` | Full design and phased roadmap. |

## License

PHP License 3.01 (same as the bundled PDO drivers). See headers in each source file.
