--TEST--
pdo_duckdb: UUID and ENUM fetch as strings
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$db->exec("CREATE TYPE mood AS ENUM ('sad', 'ok', 'happy')");
$row = $db->query(
    "SELECT '12345678-1234-5678-1234-567812345678'::UUID AS u, 'happy'::mood AS m"
)->fetch(PDO::FETCH_ASSOC);
var_dump($row);
?>
--EXPECT--
array(2) {
  ["u"]=>
  string(36) "12345678-1234-5678-1234-567812345678"
  ["m"]=>
  string(5) "happy"
}
