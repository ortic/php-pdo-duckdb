dnl config.m4 for extension pdo_duckdb

PHP_ARG_WITH([pdo-duckdb],
  [for DuckDB support for PDO],
  [AS_HELP_STRING([--with-pdo-duckdb@<:@=DIR@:>@],
    [PDO: DuckDB support. DIR is the base directory of the DuckDB C library
     (the one containing duckdb.h and libduckdb.so, or include/ and lib/
     subdirectories). Defaults to searching the bundled third_party/duckdb,
     then /usr/local and /usr.])],
  [$PHP_PDO])

if test "$PHP_PDO_DUCKDB" != "no"; then

  if test "$PHP_PDO" = "no" && test "$ext_shared" = "no"; then
    AC_MSG_ERROR([PDO is not enabled! Add --enable-pdo to your configure line.])
  fi

  PHP_CHECK_PDO_INCLUDES

  dnl Where to look for the DuckDB C library.
  if test "$PHP_PDO_DUCKDB" = "yes"; then
    PDO_DUCKDB_SEARCH="$abs_srcdir/third_party/duckdb $srcdir/third_party/duckdb /usr/local /usr"
  else
    PDO_DUCKDB_SEARCH="$PHP_PDO_DUCKDB $PHP_PDO_DUCKDB/duckdb"
  fi

  AC_MSG_CHECKING([for duckdb.h])
  PDO_DUCKDB_INC_DIR=""
  for i in $PDO_DUCKDB_SEARCH; do
    if test -r "$i/duckdb.h"; then
      PDO_DUCKDB_INC_DIR="$i"; break
    elif test -r "$i/include/duckdb.h"; then
      PDO_DUCKDB_INC_DIR="$i/include"; break
    fi
  done
  if test -z "$PDO_DUCKDB_INC_DIR"; then
    AC_MSG_RESULT([not found])
    AC_MSG_ERROR([Cannot find duckdb.h. Install the DuckDB C library or pass --with-pdo-duckdb=DIR.])
  fi
  AC_MSG_RESULT([$PDO_DUCKDB_INC_DIR])

  AC_MSG_CHECKING([for libduckdb])
  PDO_DUCKDB_LIB_DIR=""
  for i in $PDO_DUCKDB_SEARCH; do
    if test -r "$i/libduckdb.so" || test -r "$i/libduckdb.dylib"; then
      PDO_DUCKDB_LIB_DIR="$i"; break
    elif test -r "$i/lib/libduckdb.so" || test -r "$i/lib/libduckdb.dylib"; then
      PDO_DUCKDB_LIB_DIR="$i/lib"; break
    fi
  done
  if test -z "$PDO_DUCKDB_LIB_DIR"; then
    AC_MSG_RESULT([not found])
    AC_MSG_ERROR([Cannot find libduckdb. Pass --with-pdo-duckdb=DIR pointing at the DuckDB C library.])
  fi
  AC_MSG_RESULT([$PDO_DUCKDB_LIB_DIR])

  PHP_ADD_INCLUDE([$PDO_DUCKDB_INC_DIR])
  PHP_ADD_LIBRARY_WITH_PATH([duckdb], [$PDO_DUCKDB_LIB_DIR], [PDO_DUCKDB_SHARED_LIBADD])

  PHP_CHECK_LIBRARY([duckdb], [duckdb_open],
    [],
    [AC_MSG_ERROR([libduckdb found but duckdb_open is missing — is it a valid DuckDB C library?])],
    [-L$PDO_DUCKDB_LIB_DIR])

  PHP_SUBST([PDO_DUCKDB_SHARED_LIBADD])

  PHP_NEW_EXTENSION([pdo_duckdb],
    [pdo_duckdb.c duckdb_driver.c duckdb_statement.c],
    [$ext_shared],,
    [-I$pdo_cv_inc_path])

  PHP_ADD_EXTENSION_DEP(pdo_duckdb, pdo)
fi
