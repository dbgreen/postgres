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
    SET test_guc.counter = '42';
    SELECT get_counter_value();
});
is($result, '42', 'Counter value set correctly');

$result = $node->safe_psql('postgres', q{
    SET test_guc.counter = '42';
    SELECT get_counter_description();
});
like($result, qr/Count is 42/, 'Counter description correct');

$result = $node->safe_psql('postgres', q{
    SET test_guc.counter = '';
    SELECT get_counter_value();
});
is($result, '0', 'Empty counter returns 0');

$result = $node->safe_psql('postgres', q{
    SET test_guc.counter = '';
    SELECT get_counter_description();
});
like($result, qr/No counter configured/, 'Empty counter description');

$node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.counter = '100';
    ROLLBACK;
});

$result = $node->safe_psql('postgres', 'SELECT get_counter_value();');
is($result, '0', 'Counter rolled back correctly');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.counter = '10';
    SET LOCAL test_guc.counter = '20';
    SELECT get_counter_value();
});
is($result, '20', 'Second SET LOCAL replaced first');

$node->safe_psql('postgres', 'ROLLBACK');

$node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.counter = '50';
    COMMIT;
});

$result = $node->safe_psql('postgres', 'SELECT get_counter_value();');
is($result, '0', 'SET LOCAL does not persist after COMMIT');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.counter = '5';
    SAVEPOINT sp1;
    SET LOCAL test_guc.counter = '15';
    SELECT get_counter_value();
});
is($result, '15', 'Inner savepoint value active');

$node->safe_psql('postgres', 'ROLLBACK');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.counter = '5';
    SAVEPOINT sp1;
    SET LOCAL test_guc.counter = '15';
    ROLLBACK TO sp1;
    SELECT get_counter_value();
});
is($result, '5', 'Rolled back to outer savepoint value');

$node->safe_psql('postgres', 'ROLLBACK');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.counter = '111';
    SAVEPOINT sp1;
    SET LOCAL test_guc.counter = '222';
    ROLLBACK TO sp1;
    SELECT get_counter_value();
});
is($result, '111', 'After rollback, shows old value (111 not 222)');

$node->safe_psql('postgres', 'ROLLBACK');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.counter = '1';
    SAVEPOINT sp1;
    SET LOCAL test_guc.counter = '2';
    SAVEPOINT sp2;
    SET LOCAL test_guc.counter = '3';
    SAVEPOINT sp3;
    SET LOCAL test_guc.counter = '4';
    ROLLBACK TO sp1;
    SELECT get_counter_value();
});
is($result, '1', 'Rolled back to sp1 shows value 1');

$node->safe_psql('postgres', 'ROLLBACK');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.counter = '100';
    SAVEPOINT sp1;
    SET LOCAL test_guc.counter = '200';
    RELEASE SAVEPOINT sp1;
    SELECT get_counter_value();
});
is($result, '200', 'Value persists after RELEASE SAVEPOINT');

$node->safe_psql('postgres', 'ROLLBACK');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc.counter = '100';
    SAVEPOINT sp1;
    SET LOCAL test_guc.counter = '200';
    RELEASE SAVEPOINT sp1;
    COMMIT;
    SELECT get_counter_value();
});
is($result, '0', 'Released savepoint value does not persist after COMMIT');

$node->safe_psql('postgres', q{
    SET test_guc.counter = '999';
});

$node->restart;

$result = $node->safe_psql('postgres', q{
    SET test_guc.counter = '';
    SELECT get_counter_value();
});
is($result, '0', 'Counter does not persist across restart');

my $session1 = $node->background_psql('postgres');
my $session2 = $node->background_psql('postgres');

$session1->query_safe(q{SET test_guc.counter = '111';});
$session2->query_safe(q{SET test_guc.counter = '222';});

my ($stdout1, $stderr1) = $session1->query_safe('SELECT get_counter_value();');
my ($stdout2, $stderr2) = $session2->query_safe('SELECT get_counter_value();');

like($stdout1, qr/111/, 'Session 1 has its own counter value');
unlike($stdout1, qr/222/, 'Session 1 does not see session 2 value');

like($stdout2, qr/222/, 'Session 2 has its own counter value');
unlike($stdout2, qr/111/, 'Session 2 does not see session 1 value');

$session1->quit;
$session2->quit;

done_testing();
