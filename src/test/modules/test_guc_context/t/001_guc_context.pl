#!/usr/bin/perl

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $psql = $node->background_psql('postgres');

$psql->query_safe('CREATE EXTENSION test_guc_context;');
pass('extension created');

$psql->query_safe("SET test_guc_context.value = 'foo,bar,baz';");
is($psql->query_safe('SELECT test_guc_context_get_count();'), '3', 
   'item count correct for basic list');
is($psql->query_safe('SELECT test_guc_context_get_description();'), 
   'Contains 3 items', 'description correct');
is($psql->query_safe('SELECT test_guc_context_get_list();'), 
   'foo,bar,baz', 'list reconstructed correctly');

$psql->query_safe("SET test_guc_context.value = 'one,two';");
is($psql->query_safe('SELECT test_guc_context_get_count();'), '2', 
   'item count after update');

$psql->query_safe('RESET test_guc_context.value;');
is($psql->query_safe('SELECT test_guc_context_get_count() IS NULL;'), 
   't', 'RESET cleans up context (returns NULL)');

$psql->query_safe("SET test_guc_context.value = '';");
is($psql->query_safe('SELECT test_guc_context_get_count() IS NULL;'), 
   't', 'empty string handled correctly');

$psql->query_safe("SET test_guc_context.value = 'base';");
$psql->query_safe('BEGIN;');
$psql->query_safe("SET LOCAL test_guc_context.value = 'local,value';");
is($psql->query_safe('SELECT test_guc_context_get_count();'), '2',
   'SET LOCAL value active in transaction');
$psql->query_safe('COMMIT;');
is($psql->query_safe('SELECT test_guc_context_get_count();'), '1',
   'SET LOCAL reverted after commit');

$psql->query_safe("SET test_guc_context.value = 'before,rollback';");
$psql->query_safe('BEGIN;');
$psql->query_safe("SET LOCAL test_guc_context.value = 'will,be,rolled,back';");
is($psql->query_safe('SELECT test_guc_context_get_count();'), '4',
   'SET LOCAL value before rollback');
$psql->query_safe('ROLLBACK;');
is($psql->query_safe('SELECT test_guc_context_get_count();'), '2',
   'SET LOCAL context cleaned up after rollback');

$psql->query_safe('BEGIN;');
$psql->query_safe("SET test_guc_context.value = 'level0';");
$psql->query_safe('SAVEPOINT sp1;');
$psql->query_safe("SET LOCAL test_guc_context.value = 'level1,item2';");
is($psql->query_safe('SELECT test_guc_context_get_count();'), '2',
   'savepoint level 1 value');
   
$psql->query_safe('SAVEPOINT sp2;');
$psql->query_safe("SET LOCAL test_guc_context.value = 'level2,item2,item3';");
is($psql->query_safe('SELECT test_guc_context_get_count();'), '3',
   'savepoint level 2 value');
   
$psql->query_safe('ROLLBACK TO sp2;');
is($psql->query_safe('SELECT test_guc_context_get_count();'), '2',
   'rollback to savepoint sp2 restores level1 value');
   
$psql->query_safe('ROLLBACK TO sp1;');
is($psql->query_safe('SELECT test_guc_context_get_count();'), '1',
   'rollback to savepoint sp1 restores level0 value');
   
$psql->query_safe('COMMIT;');
is($psql->query_safe('SELECT test_guc_context_get_count();'), '1',
   'transaction committed preserves level0 value');

$psql->query_safe("SET test_guc_context.value = 'discard,test';");
$psql->query_safe('DISCARD ALL;');
is($psql->query_safe('SELECT test_guc_context_get_count() IS NULL;'), 
   't', 'DISCARD ALL cleaned up context');

for my $i (1..50) {
    $psql->query_safe("SET test_guc_context.value = 'iteration,number,$i';");
}
$psql->query_safe('RESET test_guc_context.value;');
pass('50 set/reset cycles completed (no crash = no memory leak)');

$psql->query_safe("SET test_guc_context.value = ' space , test , here ';");
is($psql->query_safe('SELECT test_guc_context_get_list();'), 
   'space,test,here', 'whitespace trimming works');

$psql->query_safe("SET test_guc_context.value = 'single';");
is($psql->query_safe('SELECT test_guc_context_get_count();'), '1',
   'single item count');
is($psql->query_safe('SELECT test_guc_context_get_list();'), 
   'single', 'single item list');

my $many_items = join(',', map { "item$_" } (1..100));
$psql->query_safe("SET test_guc_context.value = '$many_items';");
is($psql->query_safe('SELECT test_guc_context_get_count();'), '100',
   '100 items handled correctly');

$psql->quit;

my $session1 = $node->background_psql('postgres');
my $session2 = $node->background_psql('postgres');

$session1->query_safe("SET test_guc_context.value = 'session,one';");
$session2->query_safe("SET test_guc_context.value = 'session,two,data';");

is($session1->query_safe('SELECT test_guc_context_get_count();'), '2',
   'session 1 has independent context');
is($session2->query_safe('SELECT test_guc_context_get_count();'), '3',
   'session 2 has independent context');

$session1->quit;
$session2->quit;

$node->append_conf('postgresql.conf',
    "test_guc_context.value = 'from,config'");
$node->restart;

my $psql2 = $node->background_psql('postgres');
is($psql2->query_safe('SELECT test_guc_context_get_count();'), '2',
   'context rebuilt from config file after restart');
$psql2->quit;

$node->safe_psql('postgres',
    "ALTER DATABASE postgres SET test_guc_context.value = 'db,setting';");

my $psql3 = $node->background_psql('postgres');
is($psql3->query_safe('SELECT test_guc_context_get_count();'), '2',
   'context from ALTER DATABASE SET works');
$psql3->quit;

$node->safe_psql('postgres',
    "ALTER DATABASE postgres RESET test_guc_context.value;");

done_testing();