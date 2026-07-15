--TEST--
pdo_duckdb: lastInsertId() returns a named sequence's current value
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE SEQUENCE s START 100');
$db->exec("CREATE TABLE u(id INTEGER DEFAULT nextval('s'), name VARCHAR)");

$db->exec("INSERT INTO u(name) VALUES ('a')");
var_dump($db->lastInsertId('s'));
$db->exec("INSERT INTO u(name) VALUES ('b')");
var_dump($db->lastInsertId('s'));

try {
    $db->lastInsertId();
    echo "no error\n";
} catch (PDOException $e) {
    echo "no-arg: ", $e->getCode(), "\n";
}
?>
--EXPECT--
string(3) "100"
string(3) "101"
no-arg: IM001
