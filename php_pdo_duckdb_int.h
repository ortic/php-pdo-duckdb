/*
  +----------------------------------------------------------------------+
  | pdo_duckdb — native PHP PDO driver for DuckDB                         |
  +----------------------------------------------------------------------+
  | Internal shared declarations.                                        |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_PDO_DUCKDB_INT_H
#define PHP_PDO_DUCKDB_INT_H

#include "duckdb.h"

/* DuckDB has no global "last error" — every failing call surfaces its own
 * message. We stash the most recent one here so PDO's error reporting
 * (errorInfo(), exceptions) can retrieve it. */
typedef struct {
	const char *file;
	int         line;
	int         errcode;   /* 0 = no error, non-zero = error */
	char       *errmsg;    /* owned; allocated with pestrdup, may be NULL */
} pdo_duckdb_error_info;

/* Per-connection (pdo_dbh) driver data. */
typedef struct {
	duckdb_database       db;
	duckdb_connection     conn;
	pdo_duckdb_error_info einfo;
	zend_bool             emulate_prepares;
} pdo_duckdb_db_handle;

/* Per-statement (pdo_stmt) driver data. */
typedef struct {
	pdo_duckdb_db_handle     *H;
	duckdb_prepared_statement prepared;
	duckdb_result             result;
	zend_bool                 has_result;
	zend_bool                 emulated;   /* prepared per-execute from substituted SQL */

	/* Column schema, cached once per execution. */
	idx_t                     column_count;
	duckdb_type              *coltypes;      /* [column_count]: logical type id per column */
	uint8_t                  *decimal_scale; /* [column_count]: scale, for DECIMAL columns */
	duckdb_type              *decimal_itype; /* [column_count]: DECIMAL storage type id */

	/* Columnar -> row cursor over the result's data chunks. */
	duckdb_data_chunk         current_chunk;
	idx_t                     chunk_size;    /* rows in current_chunk */
	idx_t                     chunk_row;     /* row currently exposed to get_col */
} pdo_duckdb_stmt;

extern const pdo_driver_t pdo_duckdb_driver;
extern const struct pdo_stmt_methods duckdb_stmt_methods;

void _pdo_duckdb_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, const char *sqlstate,
		const char *msg, const char *file, int line);

#define pdo_duckdb_error(dbh, sqlstate, msg) \
	_pdo_duckdb_error(dbh, NULL, sqlstate, msg, __FILE__, __LINE__)
#define pdo_duckdb_error_stmt(stmt, sqlstate, msg) \
	_pdo_duckdb_error((stmt)->dbh, stmt, sqlstate, msg, __FILE__, __LINE__)

/* Report the error carried by a failed duckdb_result, mapping DuckDB's
 * structured error type to an appropriate SQLSTATE. */
void _pdo_duckdb_result_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt,
		duckdb_result *result, const char *file, int line);

#define pdo_duckdb_result_error(dbh, result) \
	_pdo_duckdb_result_error(dbh, NULL, result, __FILE__, __LINE__)
#define pdo_duckdb_result_error_stmt(stmt, result) \
	_pdo_duckdb_result_error((stmt)->dbh, stmt, result, __FILE__, __LINE__)

/* Best-effort SQLSTATE for a DuckDB prepare-error message (string only). */
const char *pdo_duckdb_sqlstate_for_message(const char *msg);

#endif /* PHP_PDO_DUCKDB_INT_H */
