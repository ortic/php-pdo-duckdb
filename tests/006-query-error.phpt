--TEST--
pdo_duckdb: invalid SQL raises a PDOException with a syntax SQLSTATE
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

try {
    $db->query('SELECT FROM nope bad syntax');
    echo "query: no exception\n";
} catch (PDOException $e) {
    echo "query: caught " . $e->getCode() . "\n";
}

try {
    $db->exec('THIS IS NOT SQL');
    echo "exec: no exception\n";
} catch (PDOException $e) {
    echo "exec: caught " . $e->getCode() . "\n";
}
?>
--EXPECT--
query: caught 42601
exec: caught 42601
