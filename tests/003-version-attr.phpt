--TEST--
pdo_duckdb: client/server version attributes report the DuckDB library version
--SKIPIF--
<?php if (!extension_loaded('pdo_duckdb')) die('skip pdo_duckdb not loaded'); ?>
--FILE--
<?php
$db = new PDO('duckdb::memory:');
$client = $db->getAttribute(PDO::ATTR_CLIENT_VERSION);
$server = $db->getAttribute(PDO::ATTR_SERVER_VERSION);
var_dump(is_string($client) && $client !== '');
var_dump($client === $server);
?>
--EXPECT--
bool(true)
bool(true)
