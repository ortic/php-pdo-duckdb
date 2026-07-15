--TEST--
pdo_duckdb: named (:name) parameter binding, re-executed
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE u(id INTEGER, name VARCHAR)');
$db->exec("INSERT INTO u VALUES (1, 'amy'), (2, 'ben')");
$st = $db->prepare('SELECT name FROM u WHERE id = :id');
$st->execute([':id' => 2]);
var_dump($st->fetchColumn());
$st->execute([':id' => 1]);
var_dump($st->fetchColumn());
?>
--EXPECT--
string(3) "ben"
string(3) "amy"
