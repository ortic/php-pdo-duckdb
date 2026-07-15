# pdo_duckdb — Native PHP PDO driver for DuckDB

A plan to build a native (C) PDO driver for DuckDB, as an alternative to the
existing FFI-based binding ([satur-io/duckdb-php](https://github.com/satur-io/duckdb-php)).
The goal is first-class PDO integration so DuckDB can be used transparently by
frameworks — in particular Laravel (for tests, analytics, embedded workloads).

## 1. Goal & scope

- **Deliverable:** a loadable PHP extension `pdo_duckdb` exposing DuckDB through
  the standard `PDO` / `PDOStatement` API.
- **DSN:** `duckdb:/path/to/file.duckdb`, `duckdb::memory:`, and
  `duckdb:dbname=...;read_only=true;threads=4;...` (SQLite-style + config knobs).
- **Primary use case:** drop-in `PDO` for application code and test suites
  (e.g. Laravel's `Illuminate\Database`), where in-memory DuckDB is attractive
  for fast, isolated tests, and file-based DuckDB for analytics.
- **Non-goals (initially):** scrollable cursors, multi-rowset, async/streaming
  fetch, DuckDB extensions management surface, the Appender bulk API (these are
  later phases).

### Why native PDO over FFI
- Works through the unmodified PDO abstraction → frameworks/ORMs "just work".
- No FFI runtime enable requirement, better performance, no per-call FFI marshalling.
- Can expose a `Pdo\DuckDB` driver-specific subclass (PHP 8.4+).

## 2. How a PDO driver is structured (background)

PDO is split into a **common layer** (`ext/pdo`) and per-database **drivers**
(`pdo_sqlite`, `pdo_pgsql`, …). Our driver is a separate extension that
registers itself with the PDO core via `php_pdo_register_driver()`.

We implement two vtables (from `ext/pdo/php_pdo_driver.h`):

- **`pdo_dbh_methods`** (connection handle):
  `handle_factory`, `handle_closer`, `prepare`, `doer` (exec),
  `quoter`, `begin`/`commit`/`rollback`, `set_attribute`/`get_attribute`,
  `last_insert_id`, `fetch_err`, `check_liveness`, `in_transaction`.
- **`pdo_stmt_methods`** (statement):
  `executer`, `fetcher`, `describer`, `get_col`, `param_hook`,
  `set_attr`/`get_attr`, `get_column_meta`, `next_rowset`, `cursor_closer`.

Reference implementations to lean on: **`pdo_pgsql`** (closest — server-style
`$1` placeholders, similar type breadth) and **`pdo_sqlite`** (closest — embedded,
single-file, `:memory:`, driver-specific subclass with `createFunction`).

## 3. Mapping PDO → DuckDB C API

### 3.1 Connection
- `duckdb_open_ext(path, &db, config, &err)` with a `duckdb_config` built from
  DSN options (`access_mode=READ_ONLY`, `threads`, `max_memory`, …).
- `duckdb_connect(db, &conn)` per PDO handle. One `duckdb_connection` per
  `pdo_dbh` (DuckDB connections are **not** safe to share across threads).
- Support `PDO::ATTR_PERSISTENT` via PDO's persistent handle allocation.
- `:memory:` → open with empty/`:memory:` path.

### 3.2 Prepared statements & parameter binding  *(key design point)*
- Use PDO **native prepared statements** with `pdo_parse_params()` to rewrite
  `?` and `:name` placeholders into DuckDB's `$1…$n` (exactly the `pdo_pgsql`
  strategy). Keep the ordinal→name map for `param_hook`.
- Prepare with `duckdb_prepare()`; introspect with `duckdb_nparams()`.
- Bind in `param_hook` by 1-based index using the typed `duckdb_bind_*`
  functions:
  - `PDO_PARAM_NULL` → `duckdb_bind_null`
  - `PDO_PARAM_INT` → `duckdb_bind_int64`
  - `PDO_PARAM_BOOL` → `duckdb_bind_boolean`
  - `PDO_PARAM_STR` → `duckdb_bind_varchar_length` (DuckDB auto-casts to the
    real column type, so string binding is a safe default)
  - `PDO_PARAM_LOB` → `duckdb_bind_blob`
- Execute with `duckdb_execute_prepared()`.
- Emulated prepares (`ATTR_EMULATE_PREPARES`) as a fallback path using
  `duckdb_query()` after client-side interpolation via `quoter`.

### 3.3 Result fetching  *(key design point — columnar → row)*
DuckDB's modern API is **columnar**: `duckdb_execute_prepared` →
`duckdb_result`; then repeatedly `duckdb_fetch_chunk()` → `duckdb_data_chunk`
of up to 2048 rows; per column `duckdb_data_chunk_get_vector()` →
`duckdb_vector_get_data()` + `duckdb_vector_get_validity()` (NULL bitmask).
The old `duckdb_value_*` row API is **deprecated** — do not use it.

PDO fetches **row at a time**, so the statement handle must:
1. Hold the current data chunk + a cursor (row offset within the chunk).
2. On `fetcher`, if the cursor passes the chunk end, fetch the next chunk.
3. In `get_col`, read column `c` of the current row from the cached vector,
   decoding by the column's logical type and consulting the validity mask.

`describer` fills column metadata from `duckdb_column_count/name/logical_type`.

### 3.4 Type mapping (DuckDB → PHP)
Default policy: return native PHP scalars where lossless, strings otherwise
(documented; `ATTR_STRINGIFY_FETCHES` forces all-string).

| DuckDB type | PHP result |
|---|---|
| BOOLEAN | `bool` |
| TINYINT…BIGINT, UTINYINT…UBIGINT | `int` (fits int64) |
| HUGEINT / UHUGEINT (128-bit) | `string` |
| FLOAT / DOUBLE | `float` |
| DECIMAL | `string` (lossless) |
| VARCHAR / ENUM | `string` |
| BLOB | `string` (binary) |
| DATE / TIME / TIMESTAMP[_TZ] / INTERVAL | `string` (ISO) |
| UUID | `string` |
| LIST / STRUCT / MAP / UNION / ARRAY | `string` (JSON) initially |
| NULL (validity) | PHP `null` |

Binding covers the common inbound cases; nested-type *binding* is a later phase.

### 3.5 Transactions, IDs, errors
- `begin/commit/rollback` → `duckdb_query("BEGIN/COMMIT/ROLLBACK TRANSACTION")`;
  track autocommit; `in_transaction` reflects it. DuckDB has **no savepoints /
  nested transactions** → document; do not advertise them.
- `last_insert_id`: DuckDB has no implicit rowid. Support the sequence form
  `lastInsertId('seq_name')` → `currval('seq_name')`; the no-arg form throws a
  clear "not supported" error. Document prominently.
- `fetch_err`: translate `duckdb_result_error` / `duckdb_prepare_error` into
  SQLSTATE + driver message. Build a small DuckDB-error → SQLSTATE map.
- `quoter`: implement standard SQL single-quote doubling for the emulated path.

## 4. Build & distribution

- **Extension name:** `pdo_duckdb` (module), `Pdo\DuckDB` subclass on 8.4+.
- **Build files:** `config.m4` (+ `config.w32` for Windows) using `phpize`.
  Depends on the PDO core headers and links the DuckDB **C library**.
- **DuckDB dependency:** link the prebuilt `libduckdb` (ships as a zip with
  `duckdb.h` + `libduckdb.{so,dylib,dll}`). `--with-duckdb=DIR` to point at it;
  fall back to pkg-config / system paths. (Static linking is possible but
  produces a very large `.so`; default to dynamic + document both.)
- **Target PHP:** 8.1 → 8.4 (covers Laravel 10/11/12). Guard the PDO stmt/dbh
  vtable signature differences across 8.1–8.4 with version `#if`s.
- **Packaging:** source + `phpize` first; evaluate PECL and prebuilt binaries
  (Docker images, GitHub Release artifacts per PHP × OS) later.

## 5. Phased roadmap

**Phase 0 — Skeleton & connect (walking skeleton)**
- `config.m4`, module boilerplate, `php_pdo_register_driver`.
- DSN parsing, `handle_factory` → open/connect, `handle_closer`.
- `getAttribute` for driver name/version/client version.
- Milestone: `new PDO('duckdb::memory:')` succeeds; loads under `php -m`.

**Phase 1 — Direct query & fetch**
- `doer` (exec via `duckdb_query`), `describer`, chunk-based `fetcher`/`get_col`
  for scalar types, NULL handling.
- Milestone: `$pdo->query('SELECT 42, ...')->fetchAll()` returns correct data.

**Phase 2 — Prepared statements**
- `prepare` + `pdo_parse_params` rewrite, `param_hook` binding, `executer`.
- Named + positional params; type-aware binding.
- Milestone: parameterised INSERT/SELECT round-trip; Laravel-style `?` queries.

**Phase 3 — Transactions, errors, attributes**
- begin/commit/rollback, `in_transaction`, error→SQLSTATE mapping,
  `quoter`, `last_insert_id` (sequence form), attribute get/set incl. persistence.
- Milestone: full error semantics; `.phpt` suite green.

**Phase 4 — Type breadth & polish**
- HUGEINT/DECIMAL/UUID/temporal/nested-as-JSON decoding, column metadata
  (`getColumnMeta`), `ATTR_STRINGIFY_FETCHES`, BLOB streams.
- Milestone: type-coverage `.phpt` matrix passes.

**Phase 5 — Laravel integration package** *(separate Composer package)* — **done**
- DuckDB SQL is largely Postgres-compatible → provides a `Connection` +
  `Grammar` (extending the Postgres grammars), `Connector`, and schema builder,
  registered via a service provider. Lives in its own repo:
  **[ortic/laravel-duckdb](https://github.com/ortic/laravel-duckdb)**.
- Auto-increment is emulated with DuckDB sequences; unique/index become
  `CREATE [UNIQUE] INDEX`; introspection uses `information_schema`.
- Milestone met: migrations, query builder, transactions and Eloquent all run
  against an in-memory DuckDB (verified via Capsule).

**Phase 6 — Distribution & CI**
- CI matrix (PHP 8.1–8.4 × Linux/macOS/Windows), prebuilt artifacts, docs,
  PECL evaluation.

## 6. Testing strategy
- **`.phpt`** tests (PHP's native harness, `run-tests.php`) per feature — the
  same mechanism php-src uses for `pdo_sqlite`; primary regression net.
- Cross-check behaviour against `pdo_sqlite`/`pdo_pgsql` for API conformance.
- A higher-level PHP integration suite; later a real Laravel test-suite smoke run.
- Sanitizer builds (ASan/UBSan) and valgrind on the C paths in CI.

## 7. Key risks & open questions
- **Columnar→row impedance:** correctness of chunk cursoring, especially NULLs,
  wide strings (`duckdb_string_t` inlined vs pointer), and re-entrancy. Highest-risk area.
- **PDO vtable churn across 8.1–8.4:** signature drift needs careful `#if` guards.
- **`libduckdb` size & linking:** large binary; distribution story for users who
  can't compile (prebuilt artifacts needed).
- **Feature gaps to document up front:** no savepoints, limited `lastInsertId`,
  single writer per file, nested types surfaced as JSON.
- **DuckDB C API stability:** track the deprecation of `duckdb_value_*` and pin a
  minimum DuckDB version.
- **Concurrency model:** one connection per handle; document that sharing a
  handle across threads/processes is unsupported.

## 8. Suggested first steps
1. Stand up Phase 0 skeleton (build system + connect) against a pinned
   `libduckdb` — proves the toolchain end-to-end.
2. Implement Phase 1 fetch path — the columnar→row adapter is the core technical
   bet; validating it early de-risks everything after.
