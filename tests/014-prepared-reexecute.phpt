--TEST--
pdo_duckdb: a prepared statement can be executed repeatedly
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE t (id INTEGER)');
$db->exec('INSERT INTO t VALUES (1), (2), (3)');

$st = $db->prepare('SELECT count(*) FROM t');
$st->execute();
var_dump($st->fetchColumn());

$db->exec('INSERT INTO t VALUES (4)');
$st->execute();
var_dump($st->fetchColumn());
?>
--EXPECT--
int(3)
int(4)
