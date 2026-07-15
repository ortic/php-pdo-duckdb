--TEST--
pdo_duckdb: open a file-backed database, which is created on disk
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$file = __DIR__ . '/test_pdo_duckdb_005.duckdb';
@unlink($file);

$db = new PDO('duckdb:' . $file);
var_dump($db instanceof PDO);
$db = null; /* close */

var_dump(file_exists($file));
?>
--CLEAN--
<?php
$file = __DIR__ . '/test_pdo_duckdb_005.duckdb';
@unlink($file);
@unlink($file . '.wal');
?>
--EXPECT--
bool(true)
bool(true)
