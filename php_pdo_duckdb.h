/*
  +----------------------------------------------------------------------+
  | pdo_duckdb — native PHP PDO driver for DuckDB                         |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE.               |
  +----------------------------------------------------------------------+
*/

#ifndef PHP_PDO_DUCKDB_H
#define PHP_PDO_DUCKDB_H

extern zend_module_entry pdo_duckdb_module_entry;
#define phpext_pdo_duckdb_ptr &pdo_duckdb_module_entry

#define PHP_PDO_DUCKDB_VERSION "0.1.0-dev"

#ifdef PHP_WIN32
# define PHP_PDO_DUCKDB_API __declspec(dllexport)
#elif defined(__GNUC__) && __GNUC__ >= 4
# define PHP_PDO_DUCKDB_API __attribute__ ((visibility("default")))
#else
# define PHP_PDO_DUCKDB_API
#endif

PHP_MINIT_FUNCTION(pdo_duckdb);
PHP_MSHUTDOWN_FUNCTION(pdo_duckdb);
PHP_MINFO_FUNCTION(pdo_duckdb);

#endif /* PHP_PDO_DUCKDB_H */
