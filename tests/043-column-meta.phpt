--TEST--
pdo_duckdb: getColumnMeta reports native type and pdo_type
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$st = $db->query("SELECT 1::INTEGER AS n, 'hi' AS s, [1,2] AS lst");
foreach ([0, 1, 2] as $i) {
    $m = $st->getColumnMeta($i);
    echo $m['name'], ' ', $m['native_type'], ' ', $m['pdo_type'], "\n";
}
?>
--EXPECT--
n INTEGER 1
s VARCHAR 2
lst LIST 2
