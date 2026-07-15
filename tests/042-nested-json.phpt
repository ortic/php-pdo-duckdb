--TEST--
pdo_duckdb: nested types (LIST/STRUCT/MAP/ARRAY) fetch as JSON strings
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:', null, null, [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION]);
$row = $db->query(
    "SELECT [1,2,3] AS lst, struct_pack(a := 1, b := 'x') AS st, " .
    "MAP(['k'], [9]) AS mp, [1, NULL, 3] AS wn"
)->fetch(PDO::FETCH_ASSOC);
var_dump($row);

/* Nested output is valid JSON, including recursive structures. */
$j = $db->query("SELECT [struct_pack(id := 1, tags := ['a','b'])] AS n")->fetchColumn();
var_dump(json_decode($j, true));
?>
--EXPECT--
array(4) {
  ["lst"]=>
  string(7) "[1,2,3]"
  ["st"]=>
  string(15) "{"a":1,"b":"x"}"
  ["mp"]=>
  string(23) "[{"key":"k","value":9}]"
  ["wn"]=>
  string(10) "[1,null,3]"
}
array(1) {
  [0]=>
  array(2) {
    ["id"]=>
    int(1)
    ["tags"]=>
    array(2) {
      [0]=>
      string(1) "a"
      [1]=>
      string(1) "b"
    }
  }
}
