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
#include "zend_smart_str.h"
#include "pdo/php_pdo.h"
#include "pdo/php_pdo_driver.h"
#include "php_pdo_duckdb_int.h"

/* Forward declaration: append the JSON encoding of one vector cell. */
static void duckdb_cell_to_json(smart_str *buf, duckdb_logical_type lt,
		duckdb_vector vec, idx_t row);

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

/* Render a decimal (raw integer `v` scaled by `scale`) into out (>= 48 bytes).
 * Returns length. */
static size_t duckdb_decimal_to_buf(__int128 v, uint8_t scale, char *out)
{
	char tmp[40];
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
	} else {
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
	}
	out[p] = '\0';
	return (size_t)p;
}

/* Read a DECIMAL's raw backing integer, whose storage width depends on the
 * declared precision. */
static __int128 duckdb_decimal_raw(duckdb_type internal, void *data, idx_t row)
{
	switch (internal) {
		case DUCKDB_TYPE_SMALLINT: return ((int16_t *)data)[row];
		case DUCKDB_TYPE_INTEGER:  return ((int32_t *)data)[row];
		case DUCKDB_TYPE_BIGINT:   return ((int64_t *)data)[row];
		case DUCKDB_TYPE_HUGEINT: {
			duckdb_hugeint h = ((duckdb_hugeint *)data)[row];
			return ((__int128)h.upper << 64) | (unsigned __int128)h.lower;
		}
		default: return 0;
	}
}

#endif /* __SIZEOF_INT128__ */
/* }}} */

/* {{{ temporal / uuid formatting -> a stack buffer (>= 64 bytes) */
static size_t duckdb_fmt_date(duckdb_date_struct d, char *buf)
{
	return (size_t) snprintf(buf, 24, "%04d-%02d-%02d", d.year, d.month, d.day);
}

static size_t duckdb_fmt_time(duckdb_time_struct t, char *buf)
{
	if (t.micros) {
		return (size_t) snprintf(buf, 24, "%02d:%02d:%02d.%06d",
			t.hour, t.min, t.sec, t.micros);
	}
	return (size_t) snprintf(buf, 24, "%02d:%02d:%02d", t.hour, t.min, t.sec);
}

static size_t duckdb_fmt_timestamp(duckdb_timestamp_struct ts, char *buf)
{
	if (ts.time.micros) {
		return (size_t) snprintf(buf, 40, "%04d-%02d-%02d %02d:%02d:%02d.%06d",
			ts.date.year, ts.date.month, ts.date.day,
			ts.time.hour, ts.time.min, ts.time.sec, ts.time.micros);
	}
	return (size_t) snprintf(buf, 40, "%04d-%02d-%02d %02d:%02d:%02d",
		ts.date.year, ts.date.month, ts.date.day,
		ts.time.hour, ts.time.min, ts.time.sec);
}

static size_t duckdb_fmt_interval(duckdb_interval iv, char *buf)
{
	int64_t a = iv.micros < 0 ? -iv.micros : iv.micros;
	int neg = iv.micros < 0;
	int hh = (int)(a / 3600000000LL); a %= 3600000000LL;
	int mm = (int)(a / 60000000LL);   a %= 60000000LL;
	int ss = (int)(a / 1000000LL);
	int frac = (int)(a % 1000000LL);
	return (size_t) snprintf(buf, 64, "%d months %d days %s%02d:%02d:%02d.%06d",
		iv.months, iv.days, neg ? "-" : "", hh, mm, ss, frac);
}

static size_t duckdb_fmt_uuid(duckdb_hugeint h, char *buf)
{
	/* DuckDB stores UUID as a hugeint with the high bit flipped. */
	uint64_t hi = (uint64_t)h.upper ^ 0x8000000000000000ULL;
	uint64_t lo = h.lower;
	return (size_t) snprintf(buf, 40, "%08x-%04x-%04x-%04x-%04x%08x",
		(uint32_t)(hi >> 32),
		(uint32_t)((hi >> 16) & 0xFFFF),
		(uint32_t)(hi & 0xFFFF),
		(uint32_t)(lo >> 48),
		(uint32_t)((lo >> 32) & 0xFFFF),
		(uint32_t)(lo & 0xFFFFFFFF));
}

/* Format a temporal/uuid cell to buf (>= 64). Returns length, or -1 if `tid`
 * is not one of these types. */
static int duckdb_fmt_temporal(duckdb_type tid, void *data, idx_t row, char *buf)
{
	switch (tid) {
		case DUCKDB_TYPE_DATE:
			return (int) duckdb_fmt_date(duckdb_from_date(((duckdb_date *)data)[row]), buf);
		case DUCKDB_TYPE_TIME:
			return (int) duckdb_fmt_time(duckdb_from_time(((duckdb_time *)data)[row]), buf);
		case DUCKDB_TYPE_TIMESTAMP:
		case DUCKDB_TYPE_TIMESTAMP_TZ: {
			duckdb_timestamp v = ((duckdb_timestamp *)data)[row];
			int n = (int) duckdb_fmt_timestamp(duckdb_from_timestamp(v), buf);
			if (tid == DUCKDB_TYPE_TIMESTAMP_TZ) {
				memcpy(buf + n, "+00", 3);
				buf[n + 3] = '\0';
				n += 3;
			}
			return n;
		}
		case DUCKDB_TYPE_TIMESTAMP_S: {
			duckdb_timestamp ts; ts.micros = ((int64_t *)data)[row] * 1000000LL;
			return (int) duckdb_fmt_timestamp(duckdb_from_timestamp(ts), buf);
		}
		case DUCKDB_TYPE_TIMESTAMP_MS: {
			duckdb_timestamp ts; ts.micros = ((int64_t *)data)[row] * 1000LL;
			return (int) duckdb_fmt_timestamp(duckdb_from_timestamp(ts), buf);
		}
		case DUCKDB_TYPE_TIMESTAMP_NS: {
			duckdb_timestamp ts; ts.micros = ((int64_t *)data)[row] / 1000LL;
			return (int) duckdb_fmt_timestamp(duckdb_from_timestamp(ts), buf);
		}
		case DUCKDB_TYPE_TIME_TZ: {
			duckdb_time_tz_struct s = duckdb_from_time_tz(((duckdb_time_tz *)data)[row]);
			int n = (int) duckdb_fmt_time(s.time, buf);
			int off = s.offset, oh = off / 3600, om = (off % 3600) / 60;
			if (om < 0) om = -om;
			n += snprintf(buf + n, 12, "%+03d:%02d", oh, om);
			return n;
		}
		case DUCKDB_TYPE_INTERVAL:
			return (int) duckdb_fmt_interval(((duckdb_interval *)data)[row], buf);
		case DUCKDB_TYPE_UUID:
			return (int) duckdb_fmt_uuid(((duckdb_hugeint *)data)[row], buf);
		default:
			return -1;
	}
}

/* Look up an ENUM cell's string value (heap; free with duckdb_free). */
static char *duckdb_enum_value(duckdb_logical_type lt, void *data, idx_t row)
{
	idx_t index;
	switch (duckdb_enum_internal_type(lt)) {
		case DUCKDB_TYPE_UTINYINT:  index = ((uint8_t *)data)[row]; break;
		case DUCKDB_TYPE_USMALLINT: index = ((uint16_t *)data)[row]; break;
		case DUCKDB_TYPE_UINTEGER:  index = ((uint32_t *)data)[row]; break;
		default: index = 0; break;
	}
	return duckdb_enum_dictionary_value(lt, index);
}

/* Append a JSON-escaped string. */
static void smart_str_append_json_str(smart_str *buf, const char *s, size_t len)
{
	size_t i;
	smart_str_appendc(buf, '"');
	for (i = 0; i < len; i++) {
		unsigned char c = (unsigned char) s[i];
		switch (c) {
			case '"':  smart_str_appendl(buf, "\\\"", 2); break;
			case '\\': smart_str_appendl(buf, "\\\\", 2); break;
			case '\n': smart_str_appendl(buf, "\\n", 2); break;
			case '\r': smart_str_appendl(buf, "\\r", 2); break;
			case '\t': smart_str_appendl(buf, "\\t", 2); break;
			case '\b': smart_str_appendl(buf, "\\b", 2); break;
			case '\f': smart_str_appendl(buf, "\\f", 2); break;
			default:
				if (c < 0x20) {
					char u[8];
					int l = snprintf(u, sizeof(u), "\\u%04x", c);
					smart_str_appendl(buf, u, l);
				} else {
					smart_str_appendc(buf, (char) c);
				}
		}
	}
	smart_str_appendc(buf, '"');
}
/* }}} */

/* {{{ duckdb_cell_to_json — recursively encode one cell (used for nested types) */
static void duckdb_cell_to_json(smart_str *buf, duckdb_logical_type lt,
		duckdb_vector vec, idx_t row)
{
	duckdb_type tid = duckdb_get_type_id(lt);
	uint64_t *validity = duckdb_vector_get_validity(vec);
	void *data = duckdb_vector_get_data(vec);
	char b[64];
	int l;

	if (validity != NULL && !duckdb_validity_row_is_valid(validity, row)) {
		smart_str_appendl(buf, "null", 4);
		return;
	}

	switch (tid) {
		case DUCKDB_TYPE_BOOLEAN:
			if (((uint8_t *)data)[row]) {
				smart_str_appendl(buf, "true", 4);
			} else {
				smart_str_appendl(buf, "false", 5);
			}
			return;
		case DUCKDB_TYPE_TINYINT:   smart_str_append_long(buf, ((int8_t *)data)[row]); return;
		case DUCKDB_TYPE_SMALLINT:  smart_str_append_long(buf, ((int16_t *)data)[row]); return;
		case DUCKDB_TYPE_INTEGER:   smart_str_append_long(buf, ((int32_t *)data)[row]); return;
		case DUCKDB_TYPE_BIGINT:    smart_str_append_long(buf, (zend_long)((int64_t *)data)[row]); return;
		case DUCKDB_TYPE_UTINYINT:  smart_str_append_long(buf, ((uint8_t *)data)[row]); return;
		case DUCKDB_TYPE_USMALLINT: smart_str_append_long(buf, ((uint16_t *)data)[row]); return;
		case DUCKDB_TYPE_UINTEGER:  smart_str_append_long(buf, ((uint32_t *)data)[row]); return;
		case DUCKDB_TYPE_UBIGINT:   smart_str_append_unsigned(buf, ((uint64_t *)data)[row]); return;
		case DUCKDB_TYPE_FLOAT:
			l = snprintf(b, sizeof(b), "%.9g", (double)((float *)data)[row]);
			smart_str_appendl(buf, b, l);
			return;
		case DUCKDB_TYPE_DOUBLE:
			l = snprintf(b, sizeof(b), "%.17g", ((double *)data)[row]);
			smart_str_appendl(buf, b, l);
			return;
#ifdef __SIZEOF_INT128__
		case DUCKDB_TYPE_HUGEINT:
			l = (int) duckdb_i128_to_buf(
				((__int128)((duckdb_hugeint *)data)[row].upper << 64)
					| (unsigned __int128)((duckdb_hugeint *)data)[row].lower, b);
			smart_str_appendl(buf, b, l);
			return;
		case DUCKDB_TYPE_UHUGEINT:
			l = (int) duckdb_u128_to_buf(
				((unsigned __int128)((duckdb_uhugeint *)data)[row].upper << 64)
					| ((duckdb_uhugeint *)data)[row].lower, b);
			smart_str_appendl(buf, b, l);
			return;
		case DUCKDB_TYPE_DECIMAL:
			l = (int) duckdb_decimal_to_buf(
				duckdb_decimal_raw(duckdb_decimal_internal_type(lt), data, row),
				duckdb_decimal_scale(lt), b);
			smart_str_appendl(buf, b, l);
			return;
#endif
		case DUCKDB_TYPE_VARCHAR:
		case DUCKDB_TYPE_BLOB: {
			duckdb_string_t s = ((duckdb_string_t *)data)[row];
			uint32_t slen = s.value.inlined.length;
			const char *p = duckdb_string_is_inlined(s)
				? s.value.inlined.inlined : s.value.pointer.ptr;
			smart_str_append_json_str(buf, p, slen);
			return;
		}
		case DUCKDB_TYPE_ENUM: {
			char *v = duckdb_enum_value(lt, data, row);
			if (v) {
				smart_str_append_json_str(buf, v, strlen(v));
				duckdb_free(v);
			} else {
				smart_str_appendl(buf, "null", 4);
			}
			return;
		}
		case DUCKDB_TYPE_LIST: {
			duckdb_vector child = duckdb_list_vector_get_child(vec);
			duckdb_logical_type clt = duckdb_vector_get_column_type(child);
			duckdb_list_entry e = ((duckdb_list_entry *)data)[row];
			idx_t k;
			smart_str_appendc(buf, '[');
			for (k = 0; k < e.length; k++) {
				if (k) smart_str_appendc(buf, ',');
				duckdb_cell_to_json(buf, clt, child, e.offset + k);
			}
			smart_str_appendc(buf, ']');
			duckdb_destroy_logical_type(&clt);
			return;
		}
		case DUCKDB_TYPE_ARRAY: {
			idx_t size = duckdb_array_type_array_size(lt);
			duckdb_vector child = duckdb_array_vector_get_child(vec);
			duckdb_logical_type clt = duckdb_vector_get_column_type(child);
			idx_t k;
			smart_str_appendc(buf, '[');
			for (k = 0; k < size; k++) {
				if (k) smart_str_appendc(buf, ',');
				duckdb_cell_to_json(buf, clt, child, row * size + k);
			}
			smart_str_appendc(buf, ']');
			duckdb_destroy_logical_type(&clt);
			return;
		}
		case DUCKDB_TYPE_STRUCT: {
			idx_t n = duckdb_struct_type_child_count(lt), k;
			smart_str_appendc(buf, '{');
			for (k = 0; k < n; k++) {
				char *name = duckdb_struct_type_child_name(lt, k);
				duckdb_vector cv = duckdb_struct_vector_get_child(vec, k);
				duckdb_logical_type clt = duckdb_vector_get_column_type(cv);
				if (k) smart_str_appendc(buf, ',');
				smart_str_append_json_str(buf, name, strlen(name));
				smart_str_appendc(buf, ':');
				duckdb_cell_to_json(buf, clt, cv, row);
				duckdb_destroy_logical_type(&clt);
				duckdb_free(name);
			}
			smart_str_appendc(buf, '}');
			return;
		}
		case DUCKDB_TYPE_MAP: {
			duckdb_vector entries = duckdb_list_vector_get_child(vec);
			duckdb_vector keyv = duckdb_struct_vector_get_child(entries, 0);
			duckdb_vector valv = duckdb_struct_vector_get_child(entries, 1);
			duckdb_logical_type klt = duckdb_vector_get_column_type(keyv);
			duckdb_logical_type vlt = duckdb_vector_get_column_type(valv);
			duckdb_list_entry e = ((duckdb_list_entry *)data)[row];
			idx_t k;
			smart_str_appendc(buf, '[');
			for (k = 0; k < e.length; k++) {
				if (k) smart_str_appendc(buf, ',');
				smart_str_appendl(buf, "{\"key\":", 7);
				duckdb_cell_to_json(buf, klt, keyv, e.offset + k);
				smart_str_appendl(buf, ",\"value\":", 9);
				duckdb_cell_to_json(buf, vlt, valv, e.offset + k);
				smart_str_appendc(buf, '}');
			}
			smart_str_appendc(buf, ']');
			duckdb_destroy_logical_type(&klt);
			duckdb_destroy_logical_type(&vlt);
			return;
		}
		default:
			/* temporal / uuid, or an as-yet-unhandled exotic type */
			l = duckdb_fmt_temporal(tid, data, row, b);
			if (l >= 0) {
				smart_str_append_json_str(buf, b, l);
			} else {
				smart_str_appendl(buf, "null", 4);
			}
			return;
	}
}
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
	if (S->collogical) {
		idx_t i;
		for (i = 0; i < S->column_count; i++) {
			duckdb_destroy_logical_type(&S->collogical[i]);
		}
		efree(S->collogical);
		S->collogical = NULL;
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
		pdo_duckdb_result_error_stmt(stmt, &S->result);
		duckdb_destroy_result(&S->result);
		return 0;
	}
	S->has_result = 1;

	cc = duckdb_column_count(&S->result);
	S->column_count = cc;

	if (cc > 0) {
		S->coltypes      = ecalloc(cc, sizeof(duckdb_type));
		S->collogical    = ecalloc(cc, sizeof(duckdb_logical_type));
		S->decimal_scale = ecalloc(cc, sizeof(uint8_t));
		S->decimal_itype = ecalloc(cc, sizeof(duckdb_type));

		for (i = 0; i < cc; i++) {
			duckdb_logical_type lt = duckdb_column_logical_type(&S->result, i);
			S->collogical[i] = lt; /* kept until reset; needed for enum/nested decoding */
			S->coltypes[i] = duckdb_get_type_id(lt);
			if (S->coltypes[i] == DUCKDB_TYPE_DECIMAL) {
				S->decimal_scale[i] = duckdb_decimal_scale(lt);
				S->decimal_itype[i] = duckdb_decimal_internal_type(lt);
			}
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
			char buf[48];
			size_t l = duckdb_decimal_to_buf(
				duckdb_decimal_raw(S->decimal_itype[colno], data, row),
				S->decimal_scale[colno], buf);
			ZVAL_STRINGL(result, buf, l);
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

		case DUCKDB_TYPE_ENUM: {
			char *v = duckdb_enum_value(S->collogical[colno], data, row);
			if (v) {
				ZVAL_STRING(result, v);
				duckdb_free(v);
			} else {
				ZVAL_NULL(result);
			}
			break;
		}

		case DUCKDB_TYPE_LIST:
		case DUCKDB_TYPE_ARRAY:
		case DUCKDB_TYPE_STRUCT:
		case DUCKDB_TYPE_MAP: {
			/* Nested types are surfaced as a JSON string. */
			smart_str buf = {0};
			duckdb_cell_to_json(&buf, S->collogical[colno], vec, row);
			smart_str_0(&buf);
			if (buf.s) {
				ZVAL_STR(result, buf.s);
			} else {
				ZVAL_EMPTY_STRING(result);
			}
			break;
		}

		default: {
			/* DATE / TIME / TIMESTAMP* / TIME_TZ / INTERVAL / UUID -> string. */
			char buf[64];
			int l = duckdb_fmt_temporal(S->coltypes[colno], data, row, buf);
			if (l >= 0) {
				ZVAL_STRINGL(result, buf, l);
			} else {
				/* BIT / BIGNUM / UNION / VARIANT and other exotic types. */
				ZVAL_NULL(result);
			}
			break;
		}
	}

	return 1;
}
/* }}} */

/* {{{ A human-readable DuckDB type name, for getColumnMeta(). */
static const char *duckdb_type_name(duckdb_type t)
{
	switch (t) {
		case DUCKDB_TYPE_BOOLEAN:      return "BOOLEAN";
		case DUCKDB_TYPE_TINYINT:      return "TINYINT";
		case DUCKDB_TYPE_SMALLINT:     return "SMALLINT";
		case DUCKDB_TYPE_INTEGER:      return "INTEGER";
		case DUCKDB_TYPE_BIGINT:       return "BIGINT";
		case DUCKDB_TYPE_UTINYINT:     return "UTINYINT";
		case DUCKDB_TYPE_USMALLINT:    return "USMALLINT";
		case DUCKDB_TYPE_UINTEGER:     return "UINTEGER";
		case DUCKDB_TYPE_UBIGINT:      return "UBIGINT";
		case DUCKDB_TYPE_HUGEINT:      return "HUGEINT";
		case DUCKDB_TYPE_UHUGEINT:     return "UHUGEINT";
		case DUCKDB_TYPE_FLOAT:        return "FLOAT";
		case DUCKDB_TYPE_DOUBLE:       return "DOUBLE";
		case DUCKDB_TYPE_DECIMAL:      return "DECIMAL";
		case DUCKDB_TYPE_VARCHAR:      return "VARCHAR";
		case DUCKDB_TYPE_BLOB:         return "BLOB";
		case DUCKDB_TYPE_DATE:         return "DATE";
		case DUCKDB_TYPE_TIME:         return "TIME";
		case DUCKDB_TYPE_TIME_TZ:      return "TIME WITH TIME ZONE";
		case DUCKDB_TYPE_TIMESTAMP:    return "TIMESTAMP";
		case DUCKDB_TYPE_TIMESTAMP_S:  return "TIMESTAMP_S";
		case DUCKDB_TYPE_TIMESTAMP_MS: return "TIMESTAMP_MS";
		case DUCKDB_TYPE_TIMESTAMP_NS: return "TIMESTAMP_NS";
		case DUCKDB_TYPE_TIMESTAMP_TZ: return "TIMESTAMP WITH TIME ZONE";
		case DUCKDB_TYPE_INTERVAL:     return "INTERVAL";
		case DUCKDB_TYPE_UUID:         return "UUID";
		case DUCKDB_TYPE_ENUM:         return "ENUM";
		case DUCKDB_TYPE_LIST:         return "LIST";
		case DUCKDB_TYPE_STRUCT:       return "STRUCT";
		case DUCKDB_TYPE_MAP:          return "MAP";
		case DUCKDB_TYPE_ARRAY:        return "ARRAY";
		case DUCKDB_TYPE_UNION:        return "UNION";
		case DUCKDB_TYPE_BIT:          return "BIT";
		default:                       return "UNKNOWN";
	}
}
/* }}} */

/* {{{ get_column_meta */
static int duckdb_stmt_col_meta(pdo_stmt_t *stmt, zend_long colno, zval *return_value)
{
	pdo_duckdb_stmt *S = (pdo_duckdb_stmt *)stmt->driver_data;
	duckdb_type t;
	enum pdo_param_type pdo_type = PDO_PARAM_STR;
	zval flags;

	if (!S->has_result || colno < 0 || (idx_t)colno >= S->column_count) {
		return FAILURE;
	}

	t = S->coltypes[colno];
	switch (t) {
		case DUCKDB_TYPE_BOOLEAN:
			pdo_type = PDO_PARAM_BOOL;
			break;
		case DUCKDB_TYPE_TINYINT:
		case DUCKDB_TYPE_SMALLINT:
		case DUCKDB_TYPE_INTEGER:
		case DUCKDB_TYPE_BIGINT:
		case DUCKDB_TYPE_UTINYINT:
		case DUCKDB_TYPE_USMALLINT:
		case DUCKDB_TYPE_UINTEGER:
			pdo_type = PDO_PARAM_INT;
			break;
		case DUCKDB_TYPE_BLOB:
			pdo_type = PDO_PARAM_LOB;
			break;
		default:
			pdo_type = PDO_PARAM_STR;
			break;
	}

	array_init(return_value);
	add_assoc_string(return_value, "native_type", (char *) duckdb_type_name(t));
	add_assoc_long(return_value, "pdo_type", pdo_type);

	if (S->collogical) {
		char *alias = duckdb_logical_type_get_alias(S->collogical[colno]);
		if (alias) {
			add_assoc_string(return_value, "duckdb:alias", alias);
			duckdb_free(alias);
		}
	}

	array_init(&flags);
	add_assoc_zval(return_value, "flags", &flags);

	return SUCCESS;
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
	duckdb_stmt_col_meta,
	NULL,   /* next_rowset */
	duckdb_stmt_cursor_closer
};
/* }}} */
