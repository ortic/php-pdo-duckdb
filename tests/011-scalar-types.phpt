--TEST--
pdo_duckdb: scalar type mapping (bool, integers, double, varchar, null)
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$row = $db->query(
    "SELECT true AS b, 42 AS i, 9223372036854775807::BIGINT AS big, " .
    "3.5::DOUBLE AS d, 'hi' AS s, NULL AS n"
)->fetch(PDO::FETCH_ASSOC);
var_dump($row);
?>
--EXPECT--
array(6) {
  ["b"]=>
  bool(true)
  ["i"]=>
  int(42)
  ["big"]=>
  int(9223372036854775807)
  ["d"]=>
  float(3.5)
  ["s"]=>
  string(2) "hi"
  ["n"]=>
  NULL
}
