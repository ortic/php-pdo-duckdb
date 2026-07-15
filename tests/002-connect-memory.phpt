--TEST--
pdo_duckdb: connect to an in-memory database
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:');
var_dump($db instanceof PDO);
var_dump($db->getAttribute(PDO::ATTR_DRIVER_NAME));
?>
--EXPECT--
bool(true)
string(6) "duckdb"
