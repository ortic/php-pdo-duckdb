/*
  +----------------------------------------------------------------------+
  | pdo_duckdb — native PHP PDO driver for DuckDB                         |
  +----------------------------------------------------------------------+
  | Statement (pdo_stmt) methods.                                        |
  |                                                                      |
  | Phase 0: stubs only. The columnar->row fetch adapter, parameter      |
  | binding and type mapping land in phases 1-4. The dtor is real so     |
  | that any driver_data allocated later is always released.             |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "pdo/php_pdo.h"
#include "pdo/php_pdo_driver.h"
#include "php_pdo_duckdb_int.h"

/* {{{ dtor — release DuckDB statement resources */
static int duckdb_stmt_dtor(pdo_stmt_t *stmt)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;

	if (S) {
		if (S->has_result) {
			duckdb_destroy_result(&S->result);
		}
		if (S->prepared) {
			duckdb_destroy_prepare(&S->prepared);
		}
		efree(S);
		stmt->driver_data = NULL;
	}
	return 1;
}
/* }}} */

static int duckdb_stmt_executer(pdo_stmt_t *stmt)
{
	/* phase 1 */
	return 0;
}

static int duckdb_stmt_fetcher(pdo_stmt_t *stmt,
		enum pdo_fetch_orientation ori, zend_long offset)
{
	/* phase 1 */
	return 0;
}

static int duckdb_stmt_describer(pdo_stmt_t *stmt, int colno)
{
	/* phase 1 */
	return 0;
}

static int duckdb_stmt_get_col(pdo_stmt_t *stmt, int colno, zval *result,
		enum pdo_param_type *type)
{
	/* phase 1 */
	return 0;
}

static int duckdb_stmt_param_hook(pdo_stmt_t *stmt,
		struct pdo_bound_param_data *param, enum pdo_param_event event_type)
{
	/* phase 2 (parameter binding) */
	return 1;
}

/* {{{ duckdb_stmt_methods */
const struct pdo_stmt_methods duckdb_stmt_methods = {
	duckdb_stmt_dtor,
	duckdb_stmt_executer,
	duckdb_stmt_fetcher,
	duckdb_stmt_describer,
	duckdb_stmt_get_col,
	duckdb_stmt_param_hook,
	NULL,   /* set_attribute */
	NULL,   /* get_attribute */
	NULL,   /* get_column_meta */
	NULL,   /* next_rowset */
	NULL    /* cursor_closer */
};
/* }}} */
