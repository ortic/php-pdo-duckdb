--TEST--
pdo_duckdb: prepare()/exec() report "not implemented" in phase 0
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);

try {
    $db->prepare('SELECT 1');
    echo "prepare: no exception\n";
} catch (PDOException $e) {
    echo "prepare: caught\n";
}

try {
    $db->exec('SELECT 1');
    echo "exec: no exception\n";
} catch (PDOException $e) {
    echo "exec: caught\n";
}
?>
--EXPECT--
prepare: caught
exec: caught
