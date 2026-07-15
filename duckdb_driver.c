/*
  +----------------------------------------------------------------------+
  | pdo_duckdb — native PHP PDO driver for DuckDB                         |
  +----------------------------------------------------------------------+
  | Database handle (pdo_dbh) methods.                                   |
  |                                                                      |
  | Phase 0 scope: connect/disconnect, attributes, error reporting,      |
  | quoting and transactions. Statement execution (preparer/doer) is     |
  | stubbed and arrives in phase 1.                                      |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <strings.h>

#include "php.h"
#include "pdo/php_pdo.h"
#include "pdo/php_pdo_driver.h"
#include "php_pdo_duckdb.h"
#include "php_pdo_duckdb_int.h"
#include "zend_exceptions.h"

/* {{{ Record an error and, at construct time, raise it as an exception.
 * DuckDB reports errors per-call, so callers pass the message explicitly. */
void _pdo_duckdb_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt, const char *sqlstate,
		const char *msg, const char *file, int line)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	pdo_duckdb_error_info *einfo = &H->einfo;
	pdo_error_type *pdo_err = stmt ? &stmt->error_code : &dbh->error_code;

	einfo->errcode = 1;
	einfo->file = file;
	einfo->line = line;

	if (einfo->errmsg) {
		pefree(einfo->errmsg, dbh->is_persistent);
		einfo->errmsg = NULL;
	}
	if (msg) {
		einfo->errmsg = pestrdup(msg, dbh->is_persistent);
	}

	strncpy(*pdo_err, sqlstate ? sqlstate : "HY000", sizeof(*pdo_err));

	/* If methods are not yet installed we are inside the constructor, so PDO
	 * will not surface the error itself — throw directly, like pdo_sqlite. */
	if (!dbh->methods) {
		pdo_throw_exception(einfo->errcode, einfo->errmsg, pdo_err);
	}
}
/* }}} */

/* {{{ Run a resultless statement (used for transaction control). */
static bool pdo_duckdb_simple_exec(pdo_dbh_t *dbh, const char *sql)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	duckdb_result result;

	if (duckdb_query(H->conn, sql, &result) == DuckDBError) {
		pdo_duckdb_error(dbh, "HY000", duckdb_result_error(&result));
		duckdb_destroy_result(&result);
		return false;
	}
	duckdb_destroy_result(&result);
	return true;
}
/* }}} */

/* {{{ closer */
static void pdo_duckdb_handle_closer(pdo_dbh_t *dbh)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;

	if (!H) {
		return;
	}
	if (H->conn) {
		duckdb_disconnect(&H->conn);
	}
	if (H->db) {
		duckdb_close(&H->db);
	}
	if (H->einfo.errmsg) {
		pefree(H->einfo.errmsg, dbh->is_persistent);
	}
	pefree(H, dbh->is_persistent);
	dbh->driver_data = NULL;
}
/* }}} */

/* {{{ preparer — build a DuckDB prepared statement for the statement handle.
 * Parameter binding lands in phase 2; for now unbound parameters simply cause
 * DuckDB to fail loudly at execute time. */
static bool pdo_duckdb_handle_preparer(pdo_dbh_t *dbh, zend_string *sql,
		pdo_stmt_t *stmt, zval *driver_options)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	pdo_duckdb_stmt *S = ecalloc(1, sizeof(pdo_duckdb_stmt));

	S->H = H;

	if (duckdb_prepare(H->conn, ZSTR_VAL(sql), &S->prepared) == DuckDBError) {
		pdo_duckdb_error(dbh, "HY000", duckdb_prepare_error(S->prepared));
		duckdb_destroy_prepare(&S->prepared);
		efree(S);
		return false;
	}

	stmt->driver_data = S;
	stmt->methods = &duckdb_stmt_methods;
	/* DuckDB understands positional "?" placeholders natively. */
	stmt->supports_placeholders = PDO_PLACEHOLDER_POSITIONAL;

	return true;
}
/* }}} */

/* {{{ doer — PDO::exec(): run a resultless statement, return affected rows */
static zend_long pdo_duckdb_handle_doer(pdo_dbh_t *dbh, const zend_string *sql)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	duckdb_result result;
	zend_long changed;

	if (duckdb_query(H->conn, ZSTR_VAL(sql), &result) == DuckDBError) {
		pdo_duckdb_error(dbh, "HY000", duckdb_result_error(&result));
		duckdb_destroy_result(&result);
		return -1;
	}

	changed = (zend_long) duckdb_rows_changed(&result);
	duckdb_destroy_result(&result);
	return changed;
}
/* }}} */

/* {{{ quoter — standard SQL: wrap in single quotes and double embedded quotes */
static zend_string *pdo_duckdb_handle_quoter(pdo_dbh_t *dbh,
		const zend_string *unquoted, enum pdo_param_type paramtype)
{
	const char *src = ZSTR_VAL(unquoted);
	size_t srclen = ZSTR_LEN(unquoted);
	size_t extra = 2; /* surrounding quotes */
	size_t i;
	zend_string *quoted;
	char *q;

	for (i = 0; i < srclen; i++) {
		if (src[i] == '\'') {
			extra++;
		}
	}

	quoted = zend_string_alloc(srclen + extra, 0);
	q = ZSTR_VAL(quoted);
	*q++ = '\'';
	for (i = 0; i < srclen; i++) {
		if (src[i] == '\'') {
			*q++ = '\'';
		}
		*q++ = src[i];
	}
	*q++ = '\'';
	*q = '\0';

	return quoted;
}
/* }}} */

/* {{{ transaction control */
static bool pdo_duckdb_handle_begin(pdo_dbh_t *dbh)
{
	return pdo_duckdb_simple_exec(dbh, "BEGIN TRANSACTION");
}

static bool pdo_duckdb_handle_commit(pdo_dbh_t *dbh)
{
	return pdo_duckdb_simple_exec(dbh, "COMMIT");
}

static bool pdo_duckdb_handle_rollback(pdo_dbh_t *dbh)
{
	return pdo_duckdb_simple_exec(dbh, "ROLLBACK");
}
/* }}} */

/* {{{ set_attribute — no driver-specific attributes are writable yet */
static bool pdo_duckdb_set_attribute(pdo_dbh_t *dbh, zend_long attr, zval *val)
{
	switch (attr) {
		case PDO_ATTR_TIMEOUT:
			/* Accepted and ignored for now; DuckDB has no per-connection
			 * busy timeout equivalent. */
			return true;
		default:
			return false;
	}
}
/* }}} */

/* {{{ get_attribute */
static int pdo_duckdb_get_attribute(pdo_dbh_t *dbh, zend_long attr, zval *return_value)
{
	switch (attr) {
		case PDO_ATTR_CLIENT_VERSION:
		case PDO_ATTR_SERVER_VERSION:
			ZVAL_STRING(return_value, (char *)duckdb_library_version());
			return 1;

		default:
			return 0;
	}
}
/* }}} */

/* {{{ fetch_err — populate errorInfo() array */
static void pdo_duckdb_fetch_error_func(pdo_dbh_t *dbh, pdo_stmt_t *stmt, zval *info)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	pdo_duckdb_error_info *einfo = &H->einfo;

	if (einfo->errcode) {
		add_next_index_long(info, einfo->errcode);
		if (einfo->errmsg) {
			add_next_index_string(info, einfo->errmsg);
		}
	}
}
/* }}} */

/* {{{ pdo_duckdb_methods */
static const struct pdo_dbh_methods pdo_duckdb_methods = {
	pdo_duckdb_handle_closer,
	pdo_duckdb_handle_preparer,
	pdo_duckdb_handle_doer,
	pdo_duckdb_handle_quoter,
	pdo_duckdb_handle_begin,
	pdo_duckdb_handle_commit,
	pdo_duckdb_handle_rollback,
	pdo_duckdb_set_attribute,
	NULL,   /* last_insert_id  — phase 3 */
	pdo_duckdb_fetch_error_func,
	pdo_duckdb_get_attribute,
	NULL,   /* check_liveness */
	NULL,   /* get_driver_methods */
	NULL,   /* persistent_shutdown */
	NULL,   /* in_transaction — use PDO's internal tracking */
	NULL    /* get_gc */
};
/* }}} */

/* {{{ handle_factory — open the database and connect */
static int pdo_duckdb_handle_factory(pdo_dbh_t *dbh, zval *driver_options)
{
	pdo_duckdb_db_handle *H;
	int ret = 0;
	int i;
	duckdb_config config = NULL;
	char *open_err = NULL;
	const char *path;

	/* DSN options. If none are supplied the raw data source is treated as a
	 * path (":memory:", "/path/to/file.duckdb", ...). */
	struct pdo_data_src_parser vars[] = {
		{ "dbname",      NULL, 0 },
		{ "access_mode", NULL, 0 },
		{ "threads",     NULL, 0 },
		{ "max_memory",  NULL, 0 },
		{ "read_only",   NULL, 0 },
	};
	int nvars = sizeof(vars) / sizeof(vars[0]);

	H = pecalloc(1, sizeof(pdo_duckdb_db_handle), dbh->is_persistent);
	H->einfo.errcode = 0;
	H->einfo.errmsg = NULL;
	dbh->driver_data = H;

	php_pdo_parse_data_source(dbh->data_source, strlen(dbh->data_source), vars, nvars);

	if (vars[0].optval && vars[0].optval[0] != '\0') {
		path = vars[0].optval;               /* explicit dbname= wins */
	} else {
		path = dbh->data_source;             /* e.g. ":memory:" or a plain path */
	}

	if (duckdb_create_config(&config) == DuckDBError) {
		pdo_duckdb_error(dbh, "HY000", "failed to create DuckDB configuration");
		goto cleanup;
	}

	if (vars[4].optval &&
			(!strcmp(vars[4].optval, "1") || !strcasecmp(vars[4].optval, "true"))) {
		duckdb_set_config(config, "access_mode", "READ_ONLY");
	}
	if (vars[1].optval) {
		duckdb_set_config(config, "access_mode", vars[1].optval);
	}
	if (vars[2].optval) {
		duckdb_set_config(config, "threads", vars[2].optval);
	}
	if (vars[3].optval) {
		duckdb_set_config(config, "max_memory", vars[3].optval);
	}

	if (duckdb_open_ext(path, &H->db, config, &open_err) == DuckDBError) {
		pdo_duckdb_error(dbh, "HY000",
			open_err ? open_err : "unable to open DuckDB database");
		goto cleanup;
	}

	if (duckdb_connect(H->db, &H->conn) == DuckDBError) {
		pdo_duckdb_error(dbh, "HY000", "unable to establish a DuckDB connection");
		goto cleanup;
	}

	/* PDO housekeeping */
	dbh->alloc_own_columns = 1;
	dbh->max_escaped_char_length = 2;
	/* Only the EXEC_PRE param event matters to us (binding, phase 1). */
	dbh->skip_param_evt = 0x7F ^ (1 << PDO_PARAM_EVT_EXEC_PRE);

	ret = 1;

cleanup:
	if (open_err) {
		duckdb_free(open_err);
	}
	if (config) {
		duckdb_destroy_config(&config);
	}
	for (i = 0; i < nvars; i++) {
		if (vars[i].freeme && vars[i].optval) {
			efree(vars[i].optval);
		}
	}
	dbh->methods = &pdo_duckdb_methods;

	return ret;
}
/* }}} */

/* {{{ the driver registration record */
const pdo_driver_t pdo_duckdb_driver = {
	PDO_DRIVER_HEADER(duckdb),
	pdo_duckdb_handle_factory
};
/* }}} */
