--TEST--
pdo_duckdb: extension loads and registers the "duckdb" driver
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
var_dump(extension_loaded('pdo'));
var_dump(extension_loaded('pdo_duckdb'));
var_dump(in_array('duckdb', PDO::getAvailableDrivers(), true));
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
