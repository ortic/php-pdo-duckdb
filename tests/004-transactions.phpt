--TEST--
pdo_duckdb: begin / commit / rollback and inTransaction tracking
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:');
var_dump($db->inTransaction());
var_dump($db->beginTransaction());
var_dump($db->inTransaction());
var_dump($db->commit());
var_dump($db->inTransaction());
var_dump($db->beginTransaction());
var_dump($db->rollBack());
var_dump($db->inTransaction());
?>
--EXPECT--
bool(false)
bool(true)
bool(true)
bool(true)
bool(false)
bool(true)
bool(true)
bool(false)
