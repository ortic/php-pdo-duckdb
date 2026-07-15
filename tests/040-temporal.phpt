--TEST--
pdo_duckdb: temporal types fetch as strings
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$row = $db->query(
    "SELECT DATE '2024-02-29' AS d, TIME '13:14:15.5' AS t, " .
    "TIMESTAMP '2024-02-29 13:14:15' AS ts, INTERVAL 90 DAY AS iv"
)->fetch(PDO::FETCH_ASSOC);
var_dump($row);
?>
--EXPECT--
array(4) {
  ["d"]=>
  string(10) "2024-02-29"
  ["t"]=>
  string(15) "13:14:15.500000"
  ["ts"]=>
  string(19) "2024-02-29 13:14:15"
  ["iv"]=>
  string(32) "0 months 90 days 00:00:00.000000"
}
