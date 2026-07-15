--TEST--
pdo_duckdb: emulated prepares allow reused named params and quote safely
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [
    PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION,
    PDO::ATTR_EMULATE_PREPARES => true,
]);
var_dump($db->getAttribute(PDO::ATTR_EMULATE_PREPARES));

$db->exec('CREATE TABLE u(id INTEGER, age INTEGER)');
$db->exec('INSERT INTO u VALUES (5, 40), (9, 25), (40, 5)');

/* A named parameter reused more than once only works with emulation. */
$st = $db->prepare('SELECT id FROM u WHERE id = :x OR age = :x ORDER BY id');
$st->execute([':x' => 5]);
print_r($st->fetchAll(PDO::FETCH_COLUMN));

/* Values are quoted, so a quote in the input cannot break out. */
$st = $db->prepare('SELECT ? AS s');
$st->execute(["o'brien"]);
var_dump($st->fetchColumn());
?>
--EXPECT--
bool(true)
Array
(
    [0] => 5
    [1] => 40
)
string(7) "o'brien"
