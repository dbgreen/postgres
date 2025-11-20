CREATE EXTENSION test_guc;

SET test_guc.counter = '42';
SELECT get_counter_value();
SELECT get_counter_description();

SET test_guc.counter = '';
SELECT get_counter_value();

SET test_guc.counter = '100';
BEGIN;
SET LOCAL test_guc.counter = '200';
SELECT get_counter_value();
ROLLBACK;
SELECT get_counter_value();

BEGIN;
SET LOCAL test_guc.counter = '10';
SAVEPOINT sp1;
SET LOCAL test_guc.counter = '20';
SELECT get_counter_value();
ROLLBACK TO sp1;
SELECT get_counter_value();
ROLLBACK;

SET test_guc.pool = 'prod:db1.example.com,db2.example.com,db3.example.com;max_connections=100;timeout=60';
SELECT count_servers();
SELECT get_pool_setting('pool_name');
SELECT get_pool_setting('max_connections');
SELECT get_pool_setting('timeout');

SELECT show_server_pool();

SET test_guc.pool = 'dev:localhost,192.168.1.10;max_connections=5;timeout=30';
SELECT count_servers();
SELECT get_pool_setting('pool_name');

SET test_guc.pool = '';
SELECT count_servers();

SET test_guc.pool = 'pool1:s1,s2;max_connections=10;timeout=20';
BEGIN;
SET LOCAL test_guc.pool = 'pool2:s3,s4,s5;max_connections=50;timeout=90';
SELECT count_servers();
SELECT get_pool_setting('pool_name');
ROLLBACK;
SELECT count_servers();
SELECT get_pool_setting('pool_name');

BEGIN;
SET LOCAL test_guc.pool = 'outer:host1,host2;max_connections=10;timeout=30';
SAVEPOINT sp1;
SET LOCAL test_guc.pool = 'inner:host3,host4,host5;max_connections=20;timeout=40';
SELECT count_servers();
SELECT get_pool_setting('pool_name');
ROLLBACK TO sp1;
SELECT count_servers();
SELECT get_pool_setting('pool_name');
ROLLBACK;

BEGIN;
SET LOCAL test_guc.pool = 'first:s1;max_connections=10;timeout=20';
SET LOCAL test_guc.pool = 'second:s1,s2;max_connections=20;timeout=30';
SELECT count_servers();
SELECT get_pool_setting('pool_name');
ROLLBACK;

DROP EXTENSION test_guc;
