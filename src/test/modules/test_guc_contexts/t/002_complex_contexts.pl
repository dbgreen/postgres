use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION test_guc_contexts;');

# Test 1: Simple plan shape
my $result = $node->safe_psql('postgres', q{
    SET test_guc_contexts.complex = 'simple:SeqScan(table1):100';
    SELECT show_plan_shapes();
});
like($result, qr/SeqScan\(table1\):100/, 'Simple plan shape parsed');

# Test 2: Count shapes
$result = $node->safe_psql('postgres', q{
    SET test_guc_contexts.complex = 'simple:SeqScan(table1):100';
    SELECT count_plan_shapes();
});
is($result, '1', 'Correct shape count');

# Test 3: Complex nested structure
$result = $node->safe_psql('postgres', q{
    SET test_guc_contexts.complex = 
        'complex:HashJoin(IndexScan(orders):50|SeqScan(customers):75):200';
    SELECT show_plan_shapes();
});
like($result, qr/HashJoin/, 'Complex nested plan shape parsed');
like($result, qr/IndexScan\(orders\)/, 'First child present');
like($result, qr/SeqScan\(customers\)/, 'Second child present');

# Test 4: Transaction with complex structure
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 
        'tx1:HashJoin(IndexScan(t1):10|IndexScan(t2):20):100';
    SELECT show_plan_shapes();
});
like($result, qr/HashJoin/, 'Transaction plan shape correct');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 'tx1:HashJoin(IndexScan(t1):10|IndexScan(t2):20):100';
    ROLLBACK;
    SELECT show_plan_shapes();
});
like($result, qr/No plan shapes/, 'Plan shape rolled back correctly');

# Test 5: COMMIT after SET LOCAL (should not persist)
$node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 'local:SeqScan(t1):100';
    COMMIT;
});

$result = $node->safe_psql('postgres', 'SELECT show_plan_shapes();');
like($result, qr/No plan shapes/, 'SET LOCAL does not persist after COMMIT');

# Test 6: Multiple SET LOCAL with complex nested structures
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 
        's1:SeqScan(big_table):1000,s2:IndexScan(small_table):10';
    SET LOCAL test_guc_contexts.complex = 
        's3:HashJoin(SeqScan(t1):100|IndexScan(t2):50):500';
    SELECT show_plan_shapes();
});
like($result, qr/HashJoin:500/, 'Second SET LOCAL replaced first (checking structure)');
like($result, qr/SeqScan\(t1\):100/, 'Second shape has correct first child');
unlike($result, qr/big_table/, 'First shape gone after replacement');

# Test 7: BUG DETECTION - Verify specific table names after rollback
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 'first:SeqScan(APPLES):100';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.complex = 'second:IndexScan(ZEBRAS):200';
    ROLLBACK TO sp1;
    SELECT show_plan_shapes();
});
like($result, qr/APPLES/, 'After rollback, shows APPLES table (OLD data)');
unlike($result, qr/ZEBRAS/, 'After rollback, does NOT show ZEBRAS table (NEW data)');

# Test 8: BUG DETECTION - Verify cost values after rollback
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 'level1:SeqScan(t1):111';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.complex = 'level2:SeqScan(t2):222';
    SAVEPOINT sp2;
    SET LOCAL test_guc_contexts.complex = 'level3:SeqScan(t3):333';
    ROLLBACK TO sp1;
    SELECT show_plan_shapes();
});
like($result, qr/:111/, 'Shows cost 111 from level1');
unlike($result, qr/:222/, 'Does not show cost 222');
unlike($result, qr/:333/, 'Does not show cost 333');

$node->safe_psql('postgres', 'ROLLBACK');

# Test 9: BUG DETECTION - Shape count after rollback
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 's1:SeqScan(t1):100,s2:SeqScan(t2):200';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.complex = 's1:SeqScan(t1):100,s2:SeqScan(t2):200,s3:SeqScan(t3):300,s4:SeqScan(t4):400';
    ROLLBACK TO sp1;
    SELECT count_plan_shapes();
});
is($result, '2', 'After rollback, count is 2 (not 4 from stale data)');

$node->safe_psql('postgres', 'ROLLBACK');

# Test 10: Single level savepoint
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 'outer:SeqScan(t1):100';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.complex = 
        'inner:NestedLoop(IndexScan(t2):10|IndexScan(t3):20):50';
    SELECT show_plan_shapes();
});
like($result, qr/NestedLoop/, 'Inner savepoint plan shape active');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 'outer:SeqScan(t1):100';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.complex = 
        'inner:NestedLoop(IndexScan(t2):10|IndexScan(t3):20):50';
    ROLLBACK TO sp1;
    SELECT show_plan_shapes();
});
like($result, qr/SeqScan\(t1\)/, 'Rolled back to outer plan shape');

# Test 11: Multiple nested savepoint levels
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 'level0:SeqScan(t0):100';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.complex = 'level1:IndexScan(t1):200';
    SAVEPOINT sp2;
    SET LOCAL test_guc_contexts.complex = 'level2:HashJoin(SeqScan(t2):50|IndexScan(t3):75):300';
    SAVEPOINT sp3;
    SET LOCAL test_guc_contexts.complex = 'level3:NestedLoop(SeqScan(t4):10|SeqScan(t5):20):400';
    SELECT show_plan_shapes();
});
like($result, qr/NestedLoop:400/, 'Deepest savepoint level active');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 'level0:SeqScan(t0):100';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.complex = 'level1:IndexScan(t1):200';
    SAVEPOINT sp2;
    SET LOCAL test_guc_contexts.complex = 'level2:HashJoin(SeqScan(t2):50|IndexScan(t3):75):300';
    SAVEPOINT sp3;
    SET LOCAL test_guc_contexts.complex = 'level3:NestedLoop(SeqScan(t4):10|SeqScan(t5):20):400';
    ROLLBACK TO sp2;
    SELECT show_plan_shapes();
});
like($result, qr/IndexScan\(t1\):200/, 'Rolled back to before sp2 (level1)');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 'level0:SeqScan(t0):100';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.complex = 'level1:IndexScan(t1):200';
    SAVEPOINT sp2;
    SET LOCAL test_guc_contexts.complex = 'level2:HashJoin(SeqScan(t2):50|IndexScan(t3):75):300';
    SAVEPOINT sp3;
    SET LOCAL test_guc_contexts.complex = 'level3:NestedLoop(SeqScan(t4):10|SeqScan(t5):20):400';
    ROLLBACK TO sp1;
    SELECT show_plan_shapes();
});
like($result, qr/SeqScan\(t0\):100/, 'Rolled back to before sp1 (level0)');

# Test 12: BUG DETECTION - Hash lookup after rollback
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 
        'first:SeqScan(t1):100,second:IndexScan(t2):200';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.complex = 
        'third:HashJoin(SeqScan(t3):300|IndexScan(t4):400):500';
    ROLLBACK TO sp1;
    SELECT lookup_plan_shape('second');
});
like($result, qr/IndexScan\(t2\):200/, 'Hash lookup works after rollback');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 
        'first:SeqScan(t1):100,second:IndexScan(t2):200';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.complex = 
        'third:HashJoin(SeqScan(t3):300|IndexScan(t4):400):500';
    ROLLBACK TO sp1;
    SELECT lookup_plan_shape('third');
});
like($result, qr/not found/, 'New shape not found after rollback (correctly deleted)');

$node->safe_psql('postgres', 'ROLLBACK');

# Test 13: RELEASE SAVEPOINT
$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 'base:SeqScan(t1):100';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.complex = 'inner:IndexScan(t2):200';
    RELEASE SAVEPOINT sp1;
    SELECT show_plan_shapes();
});
like($result, qr/IndexScan\(t2\):200/, 'Value persists after RELEASE SAVEPOINT');

$result = $node->safe_psql('postgres', q{
    BEGIN;
    SET LOCAL test_guc_contexts.complex = 'base:SeqScan(t1):100';
    SAVEPOINT sp1;
    SET LOCAL test_guc_contexts.complex = 'inner:IndexScan(t2):200';
    RELEASE SAVEPOINT sp1;
    COMMIT;
    SELECT show_plan_shapes();
});
like($result, qr/No plan shapes/, 'Released savepoint value does not persist after COMMIT');

# Test 14: Hash table lookup functionality
$result = $node->safe_psql('postgres', q{
    SET test_guc_contexts.complex = 
        'shape1:SeqScan(t1):100,shape2:IndexScan(t2):50,shape3:HashJoin(SeqScan(t3):30|IndexScan(t4):20):200';
    SELECT lookup_plan_shape('shape2');
});
like($result, qr/IndexScan\(t2\):50/, 'Hash lookup works');

$result = $node->safe_psql('postgres', q{
    SET test_guc_contexts.complex = 
        'shape1:SeqScan(t1):100,shape2:IndexScan(t2):50';
    SELECT lookup_plan_shape('nonexistent');
});
like($result, qr/not found/, 'Missing shape handled correctly');

# Test 15: Multiple nested levels
$result = $node->safe_psql('postgres', q{
    SET test_guc_contexts.complex = 
        'deep:HashJoin(HashJoin(SeqScan(t1):10|SeqScan(t2):20):50|IndexScan(t3):30):100';
    SELECT show_plan_shapes();
});
like($result, qr/HashJoin.*HashJoin.*SeqScan/s, 'Deeply nested structure parsed');

# Test 16: Server restart persistence (should not persist)
$node->safe_psql('postgres', q{
    SET test_guc_contexts.complex = 'persist_test:SeqScan(t1):100';
});

$node->restart;

$result = $node->safe_psql('postgres', q{
    SET test_guc_contexts.complex = '';
    SELECT show_plan_shapes();
});
like($result, qr/No plan shapes/, 'Shapes do not persist across restart');

# Test 17: Multiple sessions isolation
my $session1 = $node->background_psql('postgres');
my $session2 = $node->background_psql('postgres');

$session1->query_safe(q{SET test_guc_contexts.complex = 'sess1:SeqScan(t1):100';});
$session2->query_safe(q{SET test_guc_contexts.complex = 'sess2:IndexScan(t2):50';});

my ($stdout1, $stderr1) = $session1->query_safe('SELECT show_plan_shapes();');
my ($stdout2, $stderr2) = $session2->query_safe('SELECT show_plan_shapes();');

like($stdout1, qr/SeqScan\(t1\):100/, 'Session 1 has its own shapes');
unlike($stdout1, qr/IndexScan\(t2\)/, 'Session 1 does not see session 2 shapes');

like($stdout2, qr/IndexScan\(t2\):50/, 'Session 2 has its own shapes');
unlike($stdout2, qr/SeqScan\(t1\)/, 'Session 2 does not see session 1 shapes');

$session1->quit;
$session2->quit;

done_testing();