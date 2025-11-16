use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION test_guc;');

my $result = $node->safe_psql('postgres', q{
    SET test_guc.pool = 'mypool:db1.example.com,db2.example.com;max_connections=20;timeout=45';
    SELECT show_server_pool();
});
like($result, qr/Pool: mypool/, 'Pool name parsed correctly');
like($result, qr/db1\.example\.com/, 'First server present');
like($result, qr/db2\.example\.com/, 'Second server present');
like($result, qr/Max connections: 20/, 'Max connections parsed');
like($result, qr/Timeout: 45/, 'Timeout parsed');

$result = $node->safe_psql('postgres', q{
    SET test_guc.pool = 'mypool:db1.example.com,db2.example.com,db3.example.com';
    SELECT count_servers();
});
is($result, '3', 'Server count correct');

$result = $node->safe_psql('postgres', q{
    SET test_guc.pool = 'production:server1,server2;max_connections=100;timeout=60';
    SELECT get_pool_setting('pool_name');
});
is($result, 'production', 'Pool name setting retrieved');

$result = $node->safe_psql('postgres', q{
    SET test_guc.pool = 'production:server1,server2;max_connections=100;timeout=60';
    SELECT get_pool_setting('max_connections');
});
is($result, '100', 'Max connections setting retrieved');

$result = $node->safe_psql('postgres', q{
    SET test_guc.pool = 'production:server1,server2;max_connections=100;timeout=60';
    SELECT get_pool_setting('timeout');
});
is($result, '60', 'Timeout setting retrieved');

$result = $node->safe_psql('postgres', q{
    SET test_guc.pool = '';
    SELECT show_server_pool();
});
like($result, qr/No server pool configured/, 'Empty pool handled');

$result = $node->safe_psql('postgres', q{
    SET test_guc.pool = '';
    SELECT count_servers();
});
is($result, '0', 'Empty pool has zero servers');

$node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'temp:host1,host2,host3';
    ROLLBACK;
});

$result = $node->safe_psql('postgres', 'SELECT count_servers();');
is($result, '0', 'Pool rolled back correctly');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'pool1:server1';
    SET LOCAL test_guc.pool = 'pool2:server2,server3';
    SELECT count_servers();
});
is($result, '2', 'Second SET LOCAL replaced first');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'pool1:server1';
    SET LOCAL test_guc.pool = 'pool2:server2,server3';
    SELECT get_pool_setting('pool_name');
});
is($result, 'pool2', 'Second pool name active');

$node->safe_psql('postgres', 'ROLLBACK');

$node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'mypool:s1,s2,s3,s4,s5';
    COMMIT;
});

$result = $node->safe_psql('postgres', 'SELECT count_servers();');
is($result, '0', 'SET LOCAL does not persist after COMMIT');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'outer:host1,host2';
    SAVEPOINT sp1;
    SET LOCAL test_guc.pool = 'inner:host3,host4,host5';
    SELECT count_servers();
});
is($result, '3', 'Inner savepoint pool has 3 servers');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'outer:host1,host2';
    SAVEPOINT sp1;
    SET LOCAL test_guc.pool = 'inner:host3,host4,host5';
    SELECT get_pool_setting('pool_name');
});
is($result, 'inner', 'Inner savepoint pool name active');

$node->safe_psql('postgres', 'ROLLBACK');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'outer:host1,host2';
    SAVEPOINT sp1;
    SET LOCAL test_guc.pool = 'inner:host3,host4,host5';
    ROLLBACK TO sp1;
    SELECT count_servers();
});
is($result, '2', 'Rolled back to outer savepoint (2 servers)');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'outer:host1,host2';
    SAVEPOINT sp1;
    SET LOCAL test_guc.pool = 'inner:host3,host4,host5';
    ROLLBACK TO sp1;
    SELECT get_pool_setting('pool_name');
});
is($result, 'outer', 'Rolled back to outer pool name');

$node->safe_psql('postgres', 'ROLLBACK');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'poolA:s1,s2,s3';
    SAVEPOINT sp1;
    SET LOCAL test_guc.pool = 'poolB:s4,s5,s6,s7';
    ROLLBACK TO sp1;
    SELECT get_pool_setting('pool_name');
});
is($result, 'poolA', 'After rollback, shows poolA not poolB');

$node->safe_psql('postgres', 'ROLLBACK');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'p1:s1';
    SAVEPOINT sp1;
    SET LOCAL test_guc.pool = 'p2:s1,s2';
    SAVEPOINT sp2;
    SET LOCAL test_guc.pool = 'p3:s1,s2,s3';
    SAVEPOINT sp3;
    SET LOCAL test_guc.pool = 'p4:s1,s2,s3,s4';
    ROLLBACK TO sp1;
    SELECT count_servers();
});
is($result, '1', 'Rolled back to sp1 shows 1 server');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'p1:s1';
    SAVEPOINT sp1;
    SET LOCAL test_guc.pool = 'p2:s1,s2';
    SAVEPOINT sp2;
    SET LOCAL test_guc.pool = 'p3:s1,s2,s3';
    SAVEPOINT sp3;
    SET LOCAL test_guc.pool = 'p4:s1,s2,s3,s4';
    ROLLBACK TO sp1;
    SELECT get_pool_setting('pool_name');
});
is($result, 'p1', 'Rolled back to sp1 shows p1 pool');

$node->safe_psql('postgres', 'ROLLBACK');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'outer:host1';
    SAVEPOINT sp1;
    SET LOCAL test_guc.pool = 'inner:host2,host3';
    RELEASE SAVEPOINT sp1;
    SELECT count_servers();
});
is($result, '2', 'Value persists after RELEASE SAVEPOINT');

$node->safe_psql('postgres', 'ROLLBACK');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.pool = 'outer:host1';
    SAVEPOINT sp1;
    SET LOCAL test_guc.pool = 'inner:host2,host3';
    RELEASE SAVEPOINT sp1;
    COMMIT;
    SELECT count_servers();
});
is($result, '0', 'Released savepoint pool does not persist after COMMIT');

$result = $node->safe_psql('postgres', q{
    SET test_guc.pool = 'production:db1.prod,db2.prod,db3.prod,db4.prod;max_connections=200;timeout=120';
    SELECT show_server_pool();
});
like($result, qr/Servers \(4 total\)/, 'Complex pool shows 4 servers');
like($result, qr/Max connections: 200/, 'Complex pool max_connections');
like($result, qr/Timeout: 120/, 'Complex pool timeout');

$node->safe_psql('postgres', q{
    SET test_guc.pool = 'persistent:s1,s2,s3';
});

$node->restart;

$result = $node->safe_psql('postgres', q{
    SET test_guc.pool = '';
    SELECT count_servers();
});
is($result, '0', 'Pool does not persist across restart');

my $session1 = $node->background_psql('postgres');
my $session2 = $node->background_psql('postgres');

$session1->query_safe(q{SET test_guc.pool = 'pool1:s1,s2';});
$session2->query_safe(q{SET test_guc.pool = 'pool2:s3,s4,s5';});

my ($stdout1, $stderr1) = $session1->query_safe('SELECT count_servers();');
my ($stdout2, $stderr2) = $session2->query_safe('SELECT count_servers();');

like($stdout1, qr/2/, 'Session 1 has 2 servers');
unlike($stdout1, qr/3/, 'Session 1 does not see session 2 count');

like($stdout2, qr/3/, 'Session 2 has 3 servers');
unlike($stdout2, qr/2/, 'Session 2 does not see session 1 count');

($stdout1, $stderr1) = $session1->query_safe(q{SELECT get_pool_setting('pool_name');});
($stdout2, $stderr2) = $session2->query_safe(q{SELECT get_pool_setting('pool_name');});

like($stdout1, qr/pool1/, 'Session 1 has pool1');
like($stdout2, qr/pool2/, 'Session 2 has pool2');

$session1->quit;
$session2->quit;

done_testing();
