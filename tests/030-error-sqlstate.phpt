--TEST--
pdo_duckdb: DuckDB error types map to meaningful SQLSTATEs
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec('CREATE TABLE t(id INTEGER PRIMARY KEY)');
$db->exec('INSERT INTO t VALUES (1)');

function state(PDO $db, string $sql): string {
    try {
        $db->exec($sql);
        return 'no-error';
    } catch (PDOException $e) {
        return $e->getCode();
    }
}

echo state($db, 'INSERT INTO t VALUES (1)'), "\n";    // constraint violation
echo state($db, 'SELECT * FROM missing_table'), "\n"; // catalog
echo state($db, 'SELCT oops'), "\n";                  // syntax
?>
--EXPECT--
23000
42000
42601
