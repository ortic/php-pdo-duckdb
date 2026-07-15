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
#include <inttypes.h>

#include "php.h"
#include "pdo/php_pdo.h"
#include "pdo/php_pdo_driver.h"
#include "php_pdo_duckdb.h"
#include "php_pdo_duckdb_int.h"
#include "zend_exceptions.h"

/* {{{ Map DuckDB's structured error type to a SQLSTATE. */
static const char *pdo_duckdb_sqlstate_for_type(duckdb_error_type type)
{
	switch (type) {
		case DUCKDB_ERROR_CONSTRAINT:            return "23000"; /* integrity constraint */
		case DUCKDB_ERROR_CONVERSION:
		case DUCKDB_ERROR_MISMATCH_TYPE:
		case DUCKDB_ERROR_INVALID_TYPE:
		case DUCKDB_ERROR_DECIMAL:               return "22000"; /* data exception */
		case DUCKDB_ERROR_OUT_OF_RANGE:          return "22003"; /* numeric out of range */
		case DUCKDB_ERROR_DIVIDE_BY_ZERO:        return "22012"; /* division by zero */
		case DUCKDB_ERROR_SYNTAX:
		case DUCKDB_ERROR_PARSER:                return "42601"; /* syntax error */
		case DUCKDB_ERROR_CATALOG:
		case DUCKDB_ERROR_BINDER:
		case DUCKDB_ERROR_PERMISSION:            return "42000"; /* syntax/access rule */
		case DUCKDB_ERROR_TRANSACTION:           return "25000"; /* invalid txn state */
		case DUCKDB_ERROR_NOT_IMPLEMENTED:       return "0A000"; /* not supported */
		case DUCKDB_ERROR_OUT_OF_MEMORY:         return "53200"; /* out of memory */
		case DUCKDB_ERROR_CONNECTION:            return "08000"; /* connection exception */
		case DUCKDB_ERROR_IO:
		case DUCKDB_ERROR_NETWORK:
		case DUCKDB_ERROR_HTTP:                  return "58030"; /* I/O error */
		case DUCKDB_ERROR_PARAMETER_NOT_RESOLVED:
		case DUCKDB_ERROR_PARAMETER_NOT_ALLOWED: return "HY093"; /* invalid use of param */
		default:                                 return "HY000";
	}
}
/* }}} */

/* {{{ Best-effort SQLSTATE from a prepare-error message (no structured type). */
const char *pdo_duckdb_sqlstate_for_message(const char *msg)
{
	if (!msg) {
		return "HY000";
	}
	if (strstr(msg, "Parser Error") || strstr(msg, "Syntax Error")) {
		return "42601";
	}
	if (strstr(msg, "Catalog Error") || strstr(msg, "Binder Error")) {
		return "42000";
	}
	if (strstr(msg, "Constraint Error")) {
		return "23000";
	}
	if (strstr(msg, "Conversion Error")) {
		return "22000";
	}
	if (strstr(msg, "Not implemented Error")) {
		return "0A000";
	}
	return "HY000";
}
/* }}} */

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

/* {{{ Report a failed duckdb_result with a type-derived SQLSTATE. */
void _pdo_duckdb_result_error(pdo_dbh_t *dbh, pdo_stmt_t *stmt,
		duckdb_result *result, const char *file, int line)
{
	const char *sqlstate = pdo_duckdb_sqlstate_for_type(duckdb_result_error_type(result));
	_pdo_duckdb_error(dbh, stmt, sqlstate, duckdb_result_error(result), file, line);
}
/* }}} */

/* {{{ Run a resultless statement (used for transaction control). */
static bool pdo_duckdb_simple_exec(pdo_dbh_t *dbh, const char *sql)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	duckdb_result result;

	if (duckdb_query(H->conn, sql, &result) == DuckDBError) {
		pdo_duckdb_result_error(dbh, &result);
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
	zend_string *nsql = NULL;
	duckdb_state rc;

	S->H = H;
	stmt->driver_data = S;
	stmt->methods = &duckdb_stmt_methods;

	/* Emulated prepares: let PDO substitute quoted values into the SQL and
	 * re-prepare per execute (see the executer). This enables things native
	 * prepares can't do, such as reusing a named parameter. */
	if (driver_options
			? pdo_attr_lval(driver_options, PDO_ATTR_EMULATE_PREPARES, H->emulate_prepares)
			: H->emulate_prepares) {
		S->emulated = 1;
		stmt->supports_placeholders = PDO_PLACEHOLDER_NONE;
		return true;
	}

	/* DuckDB understands positional "?" placeholders natively; have PDO rewrite
	 * named ":name" placeholders into "?" and track the ordinal mapping. */
	stmt->supports_placeholders = PDO_PLACEHOLDER_POSITIONAL;

	switch (pdo_parse_params(stmt, sql, &nsql)) {
		case 1:
			/* query was rewritten; nsql is owned by us */
			break;
		case 0:
			/* nothing to rewrite; keep the original */
			nsql = zend_string_copy(sql);
			break;
		default:
			/* pdo_parse_params recorded the reason in stmt->error_code */
			strncpy(dbh->error_code, stmt->error_code, sizeof(dbh->error_code));
			efree(S);
			stmt->driver_data = NULL;
			return false;
	}

	rc = duckdb_prepare(H->conn, ZSTR_VAL(nsql), &S->prepared);
	zend_string_release(nsql);

	if (rc == DuckDBError) {
		const char *msg = duckdb_prepare_error(S->prepared);
		pdo_duckdb_error(dbh, pdo_duckdb_sqlstate_for_message(msg), msg);
		duckdb_destroy_prepare(&S->prepared);
		efree(S);
		stmt->driver_data = NULL;
		return false;
	}

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
		pdo_duckdb_result_error(dbh, &result);
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
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;

	switch (attr) {
		case PDO_ATTR_EMULATE_PREPARES:
			H->emulate_prepares = zend_is_true(val);
			return true;

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
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;

	switch (attr) {
		case PDO_ATTR_CLIENT_VERSION:
		case PDO_ATTR_SERVER_VERSION:
			ZVAL_STRING(return_value, (char *)duckdb_library_version());
			return 1;

		case PDO_ATTR_EMULATE_PREPARES:
			ZVAL_BOOL(return_value, H->emulate_prepares);
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

/* {{{ last_insert_id — DuckDB has no implicit row id, so this maps to
 * currval() of a named sequence: lastInsertId('my_seq'). */
static zend_string *pdo_duckdb_last_insert_id(pdo_dbh_t *dbh, const zend_string *name)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	zend_string *qname, *id = NULL;
	duckdb_result result;
	duckdb_data_chunk chunk;
	char *sql;

	if (name == NULL) {
		pdo_duckdb_error(dbh, "IM001",
			"PDO::lastInsertId() requires a sequence name with DuckDB, e.g. "
			"lastInsertId('my_seq'); DuckDB has no implicit auto-increment id");
		return NULL;
	}

	qname = pdo_duckdb_handle_quoter(dbh, name, PDO_PARAM_STR);
	spprintf(&sql, 0, "SELECT currval(%s)", ZSTR_VAL(qname));
	zend_string_release(qname);

	if (duckdb_query(H->conn, sql, &result) == DuckDBError) {
		pdo_duckdb_result_error(dbh, &result);
		duckdb_destroy_result(&result);
		efree(sql);
		return NULL;
	}
	efree(sql);

	chunk = duckdb_fetch_chunk(result);
	if (chunk != NULL) {
		if (duckdb_data_chunk_get_size(chunk) > 0) {
			duckdb_vector vec = duckdb_data_chunk_get_vector(chunk, 0);
			uint64_t *validity = duckdb_vector_get_validity(vec);
			if (validity == NULL || duckdb_validity_row_is_valid(validity, 0)) {
				int64_t *data = (int64_t *) duckdb_vector_get_data(vec);
				char buf[24];
				int l = snprintf(buf, sizeof(buf), "%" PRId64, data[0]);
				id = zend_string_init(buf, l, 0);
			}
		}
		duckdb_destroy_data_chunk(&chunk);
	}

	duckdb_destroy_result(&result);
	return id;
}
/* }}} */

/* {{{ check_liveness — used for persistent connections */
static zend_result pdo_duckdb_check_liveness(pdo_dbh_t *dbh)
{
	pdo_duckdb_db_handle *H = (pdo_duckdb_db_handle *)dbh->driver_data;
	duckdb_result result;
	duckdb_state rc;

	if (H == NULL || H->conn == NULL) {
		return FAILURE;
	}
	rc = duckdb_query(H->conn, "SELECT 1", &result);
	duckdb_destroy_result(&result);
	return rc == DuckDBError ? FAILURE : SUCCESS;
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
	pdo_duckdb_last_insert_id,
	pdo_duckdb_fetch_error_func,
	pdo_duckdb_get_attribute,
	pdo_duckdb_check_liveness,
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
