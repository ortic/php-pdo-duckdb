--TEST--
pdo_duckdb: DECIMAL, HUGEINT and out-of-range UBIGINT map to exact strings
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$row = $db->query(
    "SELECT 3.14 AS a, (-2.5)::DECIMAL(5,3) AS b, " .
    "170141183460469231731687303715884105727::HUGEINT AS c, " .
    "18446744073709551615::UBIGINT AS d"
)->fetch(PDO::FETCH_ASSOC);
var_dump($row);
?>
--EXPECT--
array(4) {
  ["a"]=>
  string(4) "3.14"
  ["b"]=>
  string(6) "-2.500"
  ["c"]=>
  string(39) "170141183460469231731687303715884105727"
  ["d"]=>
  string(20) "18446744073709551615"
}
