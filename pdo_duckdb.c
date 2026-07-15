/*
  +----------------------------------------------------------------------+
  | pdo_duckdb — native PHP PDO driver for DuckDB                         |
  +----------------------------------------------------------------------+
  | Module entry point: registers the driver with the PDO core.          |
  +----------------------------------------------------------------------+
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "pdo/php_pdo.h"
#include "pdo/php_pdo_driver.h"
#include "php_pdo_duckdb.h"
#include "php_pdo_duckdb_int.h"
#include "zend_exceptions.h"

/* {{{ pdo_duckdb_deps — we depend on the PDO core extension */
static const zend_module_dep pdo_duckdb_deps[] = {
	ZEND_MOD_REQUIRED("pdo")
	ZEND_MOD_END
};
/* }}} */

/* {{{ pdo_duckdb_module_entry */
zend_module_entry pdo_duckdb_module_entry = {
	STANDARD_MODULE_HEADER_EX, NULL,
	pdo_duckdb_deps,
	"pdo_duckdb",
	NULL,                        /* function entries */
	PHP_MINIT(pdo_duckdb),
	PHP_MSHUTDOWN(pdo_duckdb),
	NULL,                        /* RINIT */
	NULL,                        /* RSHUTDOWN */
	PHP_MINFO(pdo_duckdb),
	PHP_PDO_DUCKDB_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#if defined(COMPILE_DL_PDO_DUCKDB) || defined(COMPILE_DL_PDO_DUCKDB_EXTERNAL)
ZEND_GET_MODULE(pdo_duckdb)
#endif

/* {{{ PHP_MINIT_FUNCTION */
PHP_MINIT_FUNCTION(pdo_duckdb)
{
	return php_pdo_register_driver(&pdo_duckdb_driver);
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION */
PHP_MSHUTDOWN_FUNCTION(pdo_duckdb)
{
	php_pdo_unregister_driver(&pdo_duckdb_driver);
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION */
PHP_MINFO_FUNCTION(pdo_duckdb)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "PDO Driver for DuckDB", "enabled");
	php_info_print_table_row(2, "PDO Driver version", PHP_PDO_DUCKDB_VERSION);
	php_info_print_table_row(2, "DuckDB library version", duckdb_library_version());
	php_info_print_table_end();
}
/* }}} */
