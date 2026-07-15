/*
  +----------------------------------------------------------------------+
  | pdo_duckdb — native PHP PDO driver for DuckDB                         |
  +----------------------------------------------------------------------+
  | Statement (pdo_stmt) methods.                                        |
  |                                                                      |
  | Phase 1: execute a prepared statement and adapt DuckDB's columnar    |
  | result (data chunks + vectors) to PDO's row-at-a-time fetch model.   |
  |                                                                      |
  | Type coverage (phase 1): BOOLEAN, all integer widths (HUGEINT /      |
  | UHUGEINT / large UBIGINT as strings), FLOAT, DOUBLE, DECIMAL (exact  |
  | string), VARCHAR, BLOB, and SQL NULL. Temporal, UUID, and nested     |
  | types are decoded in a later phase and currently fetch as NULL.      |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <inttypes.h>

#include "php.h"
#include "php_streams.h"
#include "pdo/php_pdo.h"
#include "pdo/php_pdo_driver.h"
#include "php_pdo_duckdb_int.h"

/* {{{ 128-bit integer / decimal formatting helpers */
#ifdef __SIZEOF_INT128__

/* Format a signed 128-bit integer; buf must hold >= 41 bytes. Returns length. */
static size_t duckdb_i128_to_buf(__int128 v, char *buf)
{
	char tmp[40];
	int i = 0, neg = (v < 0);
	int pos = 0;
	unsigned __int128 u = neg ? (unsigned __int128)(-(v + 1)) + 1 : (unsigned __int128)v;

	if (u == 0) {
		tmp[i++] = '0';
	}
	while (u > 0) {
		tmp[i++] = (char)('0' + (int)(u % 10));
		u /= 10;
	}
	if (neg) {
		buf[pos++] = '-';
	}
	while (i > 0) {
		buf[pos++] = tmp[--i];
	}
	buf[pos] = '\0';
	return (size_t)pos;
}

/* Format an unsigned 128-bit integer; buf must hold >= 40 bytes. Returns length. */
static size_t duckdb_u128_to_buf(unsigned __int128 u, char *buf)
{
	char tmp[40];
	int i = 0, pos = 0;

	if (u == 0) {
		tmp[i++] = '0';
	}
	while (u > 0) {
		tmp[i++] = (char)('0' + (int)(u % 10));
		u /= 10;
	}
	while (i > 0) {
		buf[pos++] = tmp[--i];
	}
	buf[pos] = '\0';
	return (size_t)pos;
}

/* Render a decimal (raw integer `v` scaled by `scale`) as an exact string. */
static void duckdb_decimal_to_zval(zval *result, __int128 v, uint8_t scale)
{
	char tmp[40];
	char out[48];
	int n = 0, p = 0, k;
	int neg = (v < 0);
	int intdigits;
	unsigned __int128 u = neg ? (unsigned __int128)(-(v + 1)) + 1 : (unsigned __int128)v;

	if (u == 0) {
		tmp[n++] = '0';
	}
	while (u > 0) {
		tmp[n++] = (char)('0' + (int)(u % 10));
		u /= 10;
	}
	/* tmp[0] is the least-significant digit. */

	if (neg) {
		out[p++] = '-';
	}

	if (scale == 0) {
		while (n > 0) {
			out[p++] = tmp[--n];
		}
		out[p] = '\0';
		ZVAL_STRINGL(result, out, p);
		return;
	}

	intdigits = n - (int)scale;
	if (intdigits <= 0) {
		out[p++] = '0';
		out[p++] = '.';
		for (k = 0; k < -intdigits; k++) {
			out[p++] = '0';
		}
		while (n > 0) {
			out[p++] = tmp[--n];
		}
	} else {
		for (k = 0; k < intdigits; k++) {
			out[p++] = tmp[n - 1 - k];
		}
		out[p++] = '.';
		for (k = intdigits; k < n; k++) {
			out[p++] = tmp[n - 1 - k];
		}
	}
	out[p] = '\0';
	ZVAL_STRINGL(result, out, p);
}

#endif /* __SIZEOF_INT128__ */
/* }}} */

/* {{{ Release the result, its chunk cursor and cached schema (not the prepare). */
static void pdo_duckdb_stmt_reset_result(pdo_duckdb_stmt *S)
{
	if (S->current_chunk) {
		duckdb_destroy_data_chunk(&S->current_chunk);
		S->current_chunk = NULL;
	}
	if (S->has_result) {
		duckdb_destroy_result(&S->result);
		S->has_result = 0;
	}
	if (S->coltypes) {
		efree(S->coltypes);
		S->coltypes = NULL;
	}
	if (S->decimal_scale) {
		efree(S->decimal_scale);
		S->decimal_scale = NULL;
	}
	if (S->decimal_itype) {
		efree(S->decimal_itype);
		S->decimal_itype = NULL;
	}
	S->column_count = 0;
	S->chunk_size = 0;
	S->chunk_row = 0;
}
/* }}} */

/* {{{ dtor */
static int duckdb_stmt_dtor(pdo_stmt_t *stmt)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;

	if (S) {
		pdo_duckdb_stmt_reset_result(S);
		if (S->prepared) {
			duckdb_destroy_prepare(&S->prepared);
		}
		efree(S);
		stmt->driver_data = NULL;
	}
	return 1;
}
/* }}} */

/* {{{ executer — run the prepared statement and cache the result schema */
static int duckdb_stmt_executer(pdo_stmt_t *stmt)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
	idx_t cc, i;

	/* A statement can be executed more than once; start from a clean slate. */
	pdo_duckdb_stmt_reset_result(S);

	/* Emulated prepares: PDO has substituted the bound values into the query,
	 * so (re-)prepare it fresh for this execution. */
	if (S->emulated) {
		zend_string *q = stmt->active_query_string
			? stmt->active_query_string : stmt->query_string;
		if (S->prepared) {
			duckdb_destroy_prepare(&S->prepared);
			S->prepared = NULL;
		}
		if (duckdb_prepare(S->H->conn, ZSTR_VAL(q), &S->prepared) == DuckDBError) {
			pdo_duckdb_error_stmt(stmt, "HY000", duckdb_prepare_error(S->prepared));
			duckdb_destroy_prepare(&S->prepared);
			S->prepared = NULL;
			return 0;
		}
	}

	if (duckdb_execute_prepared(S->prepared, &S->result) == DuckDBError) {
		pdo_duckdb_error_stmt(stmt, "HY000", duckdb_result_error(&S->result));
		duckdb_destroy_result(&S->result);
		return 0;
	}
	S->has_result = 1;

	cc = duckdb_column_count(&S->result);
	S->column_count = cc;

	if (cc > 0) {
		S->coltypes      = ecalloc(cc, sizeof(duckdb_type));
		S->decimal_scale = ecalloc(cc, sizeof(uint8_t));
		S->decimal_itype = ecalloc(cc, sizeof(duckdb_type));

		for (i = 0; i < cc; i++) {
			duckdb_logical_type lt = duckdb_column_logical_type(&S->result, i);
			S->coltypes[i] = duckdb_get_type_id(lt);
			if (S->coltypes[i] == DUCKDB_TYPE_DECIMAL) {
				S->decimal_scale[i] = duckdb_decimal_scale(lt);
				S->decimal_itype[i] = duckdb_decimal_internal_type(lt);
			}
			duckdb_destroy_logical_type(&lt);
		}
	}

	php_pdo_stmt_set_column_count(stmt, (int)cc);

	/* Rows affected by INSERT/UPDATE/DELETE; 0 for SELECT/DDL. */
	stmt->row_count = (zend_long) duckdb_rows_changed(&S->result);

	return 1;
}
/* }}} */

/* {{{ describer — fill a column's metadata (name) */
static int duckdb_stmt_describer(pdo_stmt_t *stmt, int colno)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
	const char *name;

	if (!S->has_result || colno < 0 || (idx_t)colno >= S->column_count) {
		return 0;
	}

	name = duckdb_column_name(&S->result, (idx_t)colno);
	stmt->columns[colno].name = zend_string_init(name, strlen(name), 0);
	stmt->columns[colno].maxlen = SIZE_MAX;
	stmt->columns[colno].precision = 0;

	return 1;
}
/* }}} */

/* {{{ fetcher — advance the chunk cursor to the next row */
static int duckdb_stmt_fetcher(pdo_stmt_t *stmt,
		enum pdo_fetch_orientation ori, zend_long offset)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;

	if (!S->has_result) {
		return 0;
	}

	/* Still rows left in the current chunk. */
	if (S->current_chunk != NULL && (S->chunk_row + 1) < S->chunk_size) {
		S->chunk_row++;
		return 1;
	}

	/* Move to the next non-empty chunk. */
	if (S->current_chunk) {
		duckdb_destroy_data_chunk(&S->current_chunk);
		S->current_chunk = NULL;
	}

	for (;;) {
		duckdb_data_chunk chunk = duckdb_fetch_chunk(S->result);
		idx_t sz;

		if (chunk == NULL) {
			return 0; /* exhausted */
		}
		sz = duckdb_data_chunk_get_size(chunk);
		if (sz == 0) {
			duckdb_destroy_data_chunk(&chunk);
			continue;
		}
		S->current_chunk = chunk;
		S->chunk_size = sz;
		S->chunk_row = 0;
		return 1;
	}
}
/* }}} */

/* {{{ get_col — decode one cell of the current row into a zval */
static int duckdb_stmt_get_col(pdo_stmt_t *stmt, int colno, zval *result,
		enum pdo_param_type *type)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
	duckdb_vector vec;
	uint64_t *validity;
	void *data;
	idx_t row;

	if (!S->has_result || S->current_chunk == NULL) {
		return 0;
	}
	if (colno < 0 || (idx_t)colno >= S->column_count) {
		return 0;
	}

	row = S->chunk_row;
	vec = duckdb_data_chunk_get_vector(S->current_chunk, (idx_t)colno);
	validity = duckdb_vector_get_validity(vec);

	if (validity != NULL && !duckdb_validity_row_is_valid(validity, row)) {
		ZVAL_NULL(result);
		return 1;
	}

	data = duckdb_vector_get_data(vec);

	switch (S->coltypes[colno]) {
		case DUCKDB_TYPE_BOOLEAN:
			ZVAL_BOOL(result, ((uint8_t *)data)[row] != 0);
			break;

		case DUCKDB_TYPE_TINYINT:
			ZVAL_LONG(result, (zend_long)((int8_t *)data)[row]);
			break;
		case DUCKDB_TYPE_SMALLINT:
			ZVAL_LONG(result, (zend_long)((int16_t *)data)[row]);
			break;
		case DUCKDB_TYPE_INTEGER:
			ZVAL_LONG(result, (zend_long)((int32_t *)data)[row]);
			break;
		case DUCKDB_TYPE_BIGINT:
			ZVAL_LONG(result, (zend_long)((int64_t *)data)[row]);
			break;
		case DUCKDB_TYPE_UTINYINT:
			ZVAL_LONG(result, (zend_long)((uint8_t *)data)[row]);
			break;
		case DUCKDB_TYPE_USMALLINT:
			ZVAL_LONG(result, (zend_long)((uint16_t *)data)[row]);
			break;
		case DUCKDB_TYPE_UINTEGER:
			ZVAL_LONG(result, (zend_long)((uint32_t *)data)[row]);
			break;

		case DUCKDB_TYPE_UBIGINT: {
			uint64_t v = ((uint64_t *)data)[row];
			if (v <= (uint64_t)ZEND_LONG_MAX) {
				ZVAL_LONG(result, (zend_long)v);
			} else {
				char buf[24];
				int l = snprintf(buf, sizeof(buf), "%" PRIu64, v);
				ZVAL_STRINGL(result, buf, l);
			}
			break;
		}

		case DUCKDB_TYPE_FLOAT:
			ZVAL_DOUBLE(result, (double)((float *)data)[row]);
			break;
		case DUCKDB_TYPE_DOUBLE:
			ZVAL_DOUBLE(result, ((double *)data)[row]);
			break;

#ifdef __SIZEOF_INT128__
		case DUCKDB_TYPE_HUGEINT: {
			duckdb_hugeint h = ((duckdb_hugeint *)data)[row];
			__int128 v = ((__int128)h.upper << 64) | (unsigned __int128)h.lower;
			char buf[41];
			size_t l = duckdb_i128_to_buf(v, buf);
			ZVAL_STRINGL(result, buf, l);
			break;
		}
		case DUCKDB_TYPE_UHUGEINT: {
			duckdb_uhugeint h = ((duckdb_uhugeint *)data)[row];
			unsigned __int128 u = ((unsigned __int128)h.upper << 64) | h.lower;
			char buf[40];
			size_t l = duckdb_u128_to_buf(u, buf);
			ZVAL_STRINGL(result, buf, l);
			break;
		}
		case DUCKDB_TYPE_DECIMAL: {
			__int128 raw;
			switch (S->decimal_itype[colno]) {
				case DUCKDB_TYPE_SMALLINT:
					raw = ((int16_t *)data)[row];
					break;
				case DUCKDB_TYPE_INTEGER:
					raw = ((int32_t *)data)[row];
					break;
				case DUCKDB_TYPE_BIGINT:
					raw = ((int64_t *)data)[row];
					break;
				case DUCKDB_TYPE_HUGEINT: {
					duckdb_hugeint h = ((duckdb_hugeint *)data)[row];
					raw = ((__int128)h.upper << 64) | (unsigned __int128)h.lower;
					break;
				}
				default:
					raw = 0;
					break;
			}
			duckdb_decimal_to_zval(result, raw, S->decimal_scale[colno]);
			break;
		}
#endif /* __SIZEOF_INT128__ */

		case DUCKDB_TYPE_VARCHAR:
		case DUCKDB_TYPE_BLOB: {
			duckdb_string_t s = ((duckdb_string_t *)data)[row];
			uint32_t len = s.value.inlined.length;
			const char *p = duckdb_string_is_inlined(s)
				? s.value.inlined.inlined
				: s.value.pointer.ptr;
			ZVAL_STRINGL(result, p, len);
			break;
		}

		default:
			/* Temporal / UUID / nested / enum types arrive in a later phase. */
			ZVAL_NULL(result);
			break;
	}

	return 1;
}
/* }}} */

/* {{{ param_hook — bind parameters just before execution */
static int duckdb_stmt_param_hook(pdo_stmt_t *stmt,
		struct pdo_bound_param_data *param, enum pdo_param_event event_type)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
	zval *parameter;
	idx_t idx;
	duckdb_state rc;

	/* We only care about bound parameters, and only at the point just before
	 * execution (see skip_param_evt in the handle factory). */
	if (!param->is_param || event_type != PDO_PARAM_EVT_EXEC_PRE) {
		return 1;
	}

	if (param->paramno < 0) {
		pdo_duckdb_error_stmt(stmt, "HY093", "unresolved bound parameter");
		return 0;
	}
	idx = (idx_t)(param->paramno + 1); /* DuckDB parameters are 1-based */

	parameter = &param->parameter;
	if (Z_ISREF_P(parameter)) {
		parameter = Z_REFVAL_P(parameter);
	}

	if (Z_TYPE_P(parameter) == IS_NULL) {
		rc = duckdb_bind_null(S->prepared, idx);
	} else {
		switch (PDO_PARAM_TYPE(param->param_type)) {
			case PDO_PARAM_NULL:
				rc = duckdb_bind_null(S->prepared, idx);
				break;

			case PDO_PARAM_BOOL:
				rc = duckdb_bind_boolean(S->prepared, idx, zend_is_true(parameter));
				break;

			case PDO_PARAM_INT:
				rc = duckdb_bind_int64(S->prepared, idx, zval_get_long(parameter));
				break;

			case PDO_PARAM_LOB:
				if (Z_TYPE_P(parameter) == IS_RESOURCE) {
					php_stream *stm = NULL;
					php_stream_from_zval_no_verify(stm, parameter);
					if (!stm) {
						pdo_duckdb_error_stmt(stmt, "HY105",
							"expected a stream resource for a LOB parameter");
						return 0;
					}
					zend_string *mem = php_stream_copy_to_mem(stm, PHP_STREAM_COPY_ALL, 0);
					if (mem) {
						rc = duckdb_bind_blob(S->prepared, idx, ZSTR_VAL(mem), ZSTR_LEN(mem));
						zend_string_release(mem);
					} else {
						rc = duckdb_bind_blob(S->prepared, idx, "", 0);
					}
				} else {
					zend_string *zs = zval_get_string(parameter);
					rc = duckdb_bind_blob(S->prepared, idx, ZSTR_VAL(zs), ZSTR_LEN(zs));
					zend_string_release(zs);
				}
				break;

			case PDO_PARAM_STR:
			default: {
				/* PDO has already coerced PDO_PARAM_STR values to strings by the
				 * time we get here (see really_register_bound_param), so bind the
				 * value as VARCHAR and let DuckDB auto-cast to the column type. */
				zend_string *zs = zval_get_string(parameter);
				rc = duckdb_bind_varchar_length(S->prepared, idx,
					ZSTR_VAL(zs), ZSTR_LEN(zs));
				zend_string_release(zs);
				break;
			}
		}
	}

	if (rc == DuckDBError) {
		pdo_duckdb_error_stmt(stmt, "HY000", "failed to bind parameter");
		return 0;
	}
	return 1;
}
/* }}} */

/* {{{ cursor_closer — release the current result set */
static int duckdb_stmt_cursor_closer(pdo_stmt_t *stmt)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
	pdo_duckdb_stmt_reset_result(S);
	return 1;
}
/* }}} */

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
	duckdb_stmt_cursor_closer
};
/* }}} */
