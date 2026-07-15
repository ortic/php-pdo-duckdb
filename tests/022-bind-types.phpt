--TEST--
pdo_duckdb: typed parameter binding (INT, BOOL, STR/NULL, LOB)
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE t(id INTEGER, name VARCHAR, active BOOLEAN, data BLOB)');

$st = $db->prepare('INSERT INTO t VALUES (?, ?, ?, ?)');
$st->bindValue(1, 7, PDO::PARAM_INT);
$st->bindValue(2, null, PDO::PARAM_STR);
$st->bindValue(3, false, PDO::PARAM_BOOL);
$st->bindValue(4, "\x00\x01ABC", PDO::PARAM_LOB);
$st->execute();

$row = $db->query('SELECT id, name, active, data FROM t')->fetch(PDO::FETCH_ASSOC);
var_dump($row['id'], $row['name'], $row['active'], bin2hex($row['data']));

$st = $db->prepare('SELECT count(*) FROM t WHERE active = :a');
$st->bindValue(':a', false, PDO::PARAM_BOOL);
$st->execute();
var_dump($st->fetchColumn());
?>
--EXPECT--
int(7)
NULL
bool(false)
string(10) "0001414243"
int(1)
