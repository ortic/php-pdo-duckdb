--TEST--
pdo_duckdb: positional (?) parameter binding
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE u(id INTEGER, name VARCHAR, age INTEGER)');
$ins = $db->prepare('INSERT INTO u VALUES (?, ?, ?)');
foreach ([[1, 'amy', 30], [2, 'ben', 25], [3, 'cid', 40]] as $r) {
    $ins->execute($r);
}
$st = $db->prepare('SELECT name FROM u WHERE age >= ? ORDER BY id LIMIT ?');
$st->execute([28, 5]);
print_r($st->fetchAll(PDO::FETCH_COLUMN));
?>
--EXPECT--
Array
(
    [0] => amy
    [1] => cid
)
