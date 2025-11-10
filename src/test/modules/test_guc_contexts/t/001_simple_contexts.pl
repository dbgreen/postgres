use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION test_guc_contexts;');

# Test 1: Basic SET and show items
my $result = $node->safe_psql('postgres', q{
    SET test_guc_contexts.simple = 'apple,banana,cherry';
    SELECT show_simple_guc_items();
});
like($result, qr/apple/, 'First item present');
like($result, qr/banana/, 'Second item present');
like($result, qr/cherry/, 'Third item present');

# Test 2: Count items
$result = $node->safe_psql('postgres', q{
    SET test_guc_contexts.simple = 'one,two,three,four';
    SELECT count_simple_guc_items();
});
is($result, '4', 'Correct item count');

# Test 3: Empty/NULL handling
$result = $node->safe_psql('postgres', q{
    SET test_guc_contexts.simple = '';
    SELECT show_simple_guc_items();
});
like($result, qr/No items/, 'Empty string handled correctly');

# Test 4: Transaction rollback
$node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'tx_item1,tx_item2';
    ROLLBACK;
});

$result = $node->safe_psql('postgres', 'SELECT show_simple_guc_items();');
like($result, qr/No items/, 'Rolled back correctly');

# Test 5: Multiple SET LOCAL at same level
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'first,second';
    SET LOCAL test_guc_contexts.simple = 'third,fourth';
    SELECT show_simple_guc_items();
});
like($result, qr/third/, 'Second SET LOCAL value active');
like($result, qr/fourth/, 'Second SET LOCAL value active');
unlike($result, qr/first/, 'First SET LOCAL value replaced');

$node->safe_psql('postgres', 'ROLLBACK');

# Test 6: SET LOCAL does not persist after COMMIT
$node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'temp1,temp2';
    COMMIT;
});

$result = $node->safe_psql('postgres', 'SELECT show_simple_guc_items();');
like($result, qr/No items/, 'SET LOCAL does not persist after COMMIT');

# Test 7: Savepoint behavior
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'outer';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.simple = 'inner1,inner2';
    SELECT show_simple_guc_items();
});
like($result, qr/inner1/, 'Inner savepoint value active');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'outer';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.simple = 'inner1,inner2';
    ROLLBACK TO sp1;
    SELECT show_simple_guc_items();
});
like($result, qr/outer/, 'Rolled back to outer savepoint value');

$node->safe_psql('postgres', 'ROLLBACK');

# Test 8: BUG DETECTION - Verify actual data content after rollback
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'LEVEL_ONE_DATA';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.simple = 'LEVEL_TWO_DATA';
    ROLLBACK TO sp1;
    SELECT show_simple_guc_items();
});
like($result, qr/LEVEL_ONE_DATA/, 'After rollback, shows LEVEL_ONE data (not stale)');
unlike($result, qr/LEVEL_TWO_DATA/, 'After rollback, does NOT show LEVEL_TWO data');

# Test 9: BUG DETECTION - Multiple rollback levels with distinct values
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'ZERO';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.simple = 'ONE';
    SAVEPOINT sp2;
    SET LOCAL test_guc_contexts.simple = 'TWO';
    SAVEPOINT sp3;
    SET LOCAL test_guc_contexts.simple = 'THREE';
    ROLLBACK TO sp1;
    SELECT show_simple_guc_items();
});
like($result, qr/ZERO/, 'Rolled back to sp1 shows ZERO');
unlike($result, qr/THREE/, 'Does not show THREE after rollback');
unlike($result, qr/TWO/, 'Does not show TWO after rollback');
unlike($result, qr/ONE/, 'Does not show ONE after rollback');

$node->safe_psql('postgres', 'ROLLBACK');

# Test 10: BUG DETECTION - Count verification after rollback
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'a,b,c';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.simple = 'w,x,y,z';
    ROLLBACK TO sp1;
    SELECT count_simple_guc_items();
});
is($result, '3', 'After rollback, count is 3 (not 4 from stale data)');

$node->safe_psql('postgres', 'ROLLBACK');

# Test 11: Multiple nested savepoint levels
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'level0';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.simple = 'level1';
    SAVEPOINT sp2;
    SET LOCAL test_guc_contexts.simple = 'level2';
    SAVEPOINT sp3;
    SET LOCAL test_guc_contexts.simple = 'level3';
    SELECT show_simple_guc_items();
});
like($result, qr/level3/, 'Deepest savepoint level active');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'level0';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.simple = 'level1';
    SAVEPOINT sp2;
    SET LOCAL test_guc_contexts.simple = 'level2';
    SAVEPOINT sp3;
    SET LOCAL test_guc_contexts.simple = 'level3';
    ROLLBACK TO sp2;
    SELECT show_simple_guc_items();
});
like($result, qr/level1/, 'Rolled back to before sp2');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'level0';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.simple = 'level1';
    SAVEPOINT sp2;
    SET LOCAL test_guc_contexts.simple = 'level2';
    SAVEPOINT sp3;
    SET LOCAL test_guc_contexts.simple = 'level3';
    ROLLBACK TO sp1;
    SELECT show_simple_guc_items();
});
like($result, qr/level0/, 'Rolled back to before sp1');

$node->safe_psql('postgres', 'ROLLBACK');

# Test 12: RELEASE SAVEPOINT
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'base';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.simple = 'inner';
    RELEASE SAVEPOINT sp1;
    SELECT show_simple_guc_items();
});
like($result, qr/inner/, 'Value persists after RELEASE SAVEPOINT');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.simple = 'base';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.simple = 'inner';
    RELEASE SAVEPOINT sp1;
    COMMIT;
    SELECT show_simple_guc_items();
});
like($result, qr/No items/, 'Released savepoint value does not persist after COMMIT');

# Test 13: Server restart
$node->safe_psql('postgres', q{
    SET test_guc_contexts.simple = 'persist_test';
});

$node->restart;

$result = $node->safe_psql('postgres', q{
    SET test_guc_contexts.simple = '';
    SELECT show_simple_guc_items();
});
like($result, qr/No items/, 'Values do not persist across restart');

# Test 14: Session isolation
my $session1 = $node->background_psql('postgres');
my $session2 = $node->background_psql('postgres');

$session1->query_safe(q{SET test_guc_contexts.simple = 'session1_item';});
$session2->query_safe(q{SET test_guc_contexts.simple = 'session2_item';});

my ($stdout1, $stderr1) = $session1->query_safe('SELECT show_simple_guc_items();');
my ($stdout2, $stderr2) = $session2->query_safe('SELECT show_simple_guc_items();');

like($stdout1, qr/session1_item/, 'Session 1 has its own value');
unlike($stdout1, qr/session2_item/, 'Session 1 does not see session 2 value');

like($stdout2, qr/session2_item/, 'Session 2 has its own value');
unlike($stdout2, qr/session1_item/, 'Session 2 does not see session 1 value');

$session1->quit;
$session2->quit;

done_testing();