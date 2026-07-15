--TEST--
pdo_duckdb: exec() returns affected rows and query() iterates rows
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
var_dump($db->exec('CREATE TABLE t (id INTEGER, name VARCHAR)'));
var_dump($db->exec("INSERT INTO t VALUES (1,'a'),(2,'b'),(3,'c')"));
var_dump($db->exec("UPDATE t SET name='x' WHERE id <= 2"));
var_dump($db->exec("DELETE FROM t WHERE id = 3"));
foreach ($db->query('SELECT id, name FROM t ORDER BY id') as $r) {
    echo $r['id'], '=', $r['name'], "\n";
}
?>
--EXPECT--
int(0)
int(3)
int(2)
int(1)
1=x
2=x
