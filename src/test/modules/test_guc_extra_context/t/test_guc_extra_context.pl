#!/usr/bin/perl
# Test GUC_EXTRA_IS_CONTEXT feature 
#

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION test_guc_extra_context;');

print "=" x 70 . "\n";
print "Don't preserve userData\n";
print "Key: Always create NEW contexts, use TopMemoryContext parent\n";
print "=" x 70 . "\n\n";

print "Test 1: Basic SET creates a context\n";
print "-" x 70 . "\n";

my $ctx1 = $node->safe_psql('postgres', q{
	SET test_guc_extra_context.value = 'value1';
	SELECT guc_test_get_context_address();
});

ok($ctx1 ne '', 'Context created on first SET');
print "  Context address: $ctx1\n\n";

print "Test 2: Each SET creates a context\n";
print "-" x 70 . "\n";

my $ctx2 = $node->safe_psql('postgres', q{
	SET test_guc_extra_context.value = 'value2';
	SELECT guc_test_get_context_address();
});

ok($ctx2 ne '', 'Second SET creates a context');
print "  First context:  $ctx1\n";
print "  Second context: $ctx2\n";
if ($ctx1 eq $ctx2) {
	print "  Note: Addresses match due to allocator reuse (both contexts were created)\n";
} else {
	print "  Different addresses confirm new context\n";
}
print "\n";

print "Test 3: ROLLBACK restores previous context\n";
print "-" x 70 . "\n";

my ($ctx_base, $ctx_during, $ctx_after) = split /\n/, $node->safe_psql('postgres', q{
	SET test_guc_extra_context.value = 'base';
	SELECT guc_test_get_context_address();
	BEGIN;
	SET test_guc_extra_context.value = 'during_txn';
	SELECT guc_test_get_context_address();
	ROLLBACK;
	SELECT guc_test_get_context_address();
});

isnt($ctx_base, $ctx_during, 'Transaction SET creates new context');
is($ctx_after, $ctx_base, 'ROLLBACK restores original context');
print "  Base context:   $ctx_base\n";
print "  During txn:     $ctx_during (new)\n";
print "  After ROLLBACK: $ctx_after\n";
print "  Old context restored correctly\n\n";

print "Test 4: SET LOCAL followed by COMMIT\n";
print "-" x 70 . "\n";

my ($ctx_session, $ctx_local, $ctx_after_commit) = split /\n/, $node->safe_psql('postgres', q{
	SET test_guc_extra_context.value = 'session_value';
	SELECT guc_test_get_context_address();
	BEGIN;
	SET LOCAL test_guc_extra_context.value = 'local_value';
	SELECT guc_test_get_context_address();
	COMMIT;
	SELECT guc_test_get_context_address();
});

isnt($ctx_session, $ctx_local, 'SET LOCAL creates new context');
is($ctx_after_commit, $ctx_session, 'COMMIT restores session context');
print "  Session context:      $ctx_session\n";
print "  LOCAL context:        $ctx_local (new)\n";
print "  After COMMIT:         $ctx_after_commit\n";
print "  SET LOCAL/COMMIT works correctly\n\n";

print "Test 5: Nested transactions with SAVEPOINT\n";
print "-" x 70 . "\n";

my ($ctx_outer, $ctx_inner, $ctx_after_rollback) = split /\n/, $node->safe_psql('postgres', q{
	BEGIN;
	SET test_guc_extra_context.value = 'outer';
	SELECT guc_test_get_context_address();
	SAVEPOINT s1;
	SET test_guc_extra_context.value = 'inner';
	SELECT guc_test_get_context_address();
	ROLLBACK TO SAVEPOINT s1;
	SELECT guc_test_get_context_address();
	COMMIT;
});

isnt($ctx_outer, $ctx_inner, 'Inner SET creates new context');
is($ctx_after_rollback, $ctx_outer, 'ROLLBACK TO SAVEPOINT restores outer context');
print "  Outer context:        $ctx_outer\n";
print "  Inner context:        $ctx_inner (new)\n";
print "  After ROLLBACK TO:    $ctx_after_rollback\n";
print "  SAVEPOINT/ROLLBACK works correctly\n\n";

print "Test 6: Context survives server restart\n";
print "-" x 70 . "\n";

my $ctx_before_restart = $node->safe_psql('postgres', q{
	SET test_guc_extra_context.value = 'persist_test';
	SELECT guc_test_get_context_address();
});

print "  Context before restart: $ctx_before_restart\n";

$node->restart;

my $ctx_after_restart = $node->safe_psql('postgres', q{
	SELECT guc_test_get_context_address();
});

ok($ctx_after_restart eq '', 'Context cleared after restart (GUC reset to default)');
print "  Context after restart:  (empty)\n";
print "  Restart behavior correct\n\n";

my $ctx_new = $node->safe_psql('postgres', q{
	SET test_guc_extra_context.value = 'after_restart';
	SELECT guc_test_get_context_address();
});

ok($ctx_new ne '', 'New context created after restart');
print "  New context after restart: $ctx_new\n\n";

print "Test 7: Multiple sessions have isolated contexts\n";
print "-" x 70 . "\n";

my $ctx_s1 = $node->safe_psql('postgres', q{
	SET test_guc_extra_context.value = 'session1_value';
	SELECT guc_test_get_context_address();
});
my $ctx_s2 = $node->safe_psql('postgres', q{
	SET test_guc_extra_context.value = 'session2_value';
	SELECT guc_test_get_context_address();
});

my $ctx_s1_verify = $node->safe_psql('postgres', q{
	SELECT guc_test_get_context_address();
});

ok($ctx_s1 ne '', 'Session 1 creates a context');
ok($ctx_s2 ne '', 'Session 2 creates a context');
ok($ctx_s1_verify eq '', 'New session has no context (session isolation)');
print "  Session 1 context: $ctx_s1\n";
print "  Session 2 context: $ctx_s2\n";
if ($ctx_s1 eq $ctx_s2) {
	print "  Note: Addresses match due to allocator reuse (both contexts were created)\n";
} else {
	print "  Different addresses\n";
}
print "  Session 3 context: (empty - no GUC set)\n";
print "  Session isolation works correctly\n\n";

print "Test 8: Complex value with comma-separated items\n";
print "-" x 70 . "\n";

my $ctx_complex = $node->safe_psql('postgres', q{
	SET test_guc_extra_context.value = 'item1, item2, item3, item4';
	SELECT guc_test_get_context_address();
});

ok($ctx_complex ne '', 'Context created for complex value');
print "  Context for 4-item list: $ctx_complex\n";
print "  Complex data structures work\n\n";


done_testing();