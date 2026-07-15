--TEST--
pdo_duckdb: fetching spans multiple data chunks (> vector size)
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$n = 0;
$sum = 0;
foreach ($db->query('SELECT x FROM range(5000) t(x)') as $r) {
    $n++;
    $sum += $r[0];
}
var_dump($n);
var_dump($sum);
?>
--EXPECT--
int(5000)
int(12497500)
