#!/usr/bin/perl
# Copyright (c) 2025, PostgreSQL Global Development Group

=pod

=head1 NAME

001_windows_large_files.pl - Test Windows support for files >4GB

=head1 SYNOPSIS

  prove src/test/modules/test_large_files/t/001_windows_large_files.pl

=head1 DESCRIPTION

This test verifies that PostgreSQL on Windows can correctly handle file
operations at offsets beyond 4GB. This requires PostgreSQL to be
built with a segment size greater than 2GB.

The test uses sparse files to avoid actually writing gigabytes of data.

=cut

use strict;
use warnings;
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use File::Spec;
use File::Temp;

if ($^O ne 'MSWin32')
{
	plan skip_all => 'test is Windows-specific';
}

plan tests => 4;

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

$node->safe_psql('postgres', 'CREATE EXTENSION test_large_files;');
pass("test_large_files extension loaded");

my $tempdir = File::Temp->newdir();
my $testfile = File::Spec->catfile($tempdir, 'large_file_test.dat');

note "Test file: $testfile";

my $create_result = $node->safe_psql('postgres',
	"SELECT test_create_sparse_file('$testfile', 5);");
is($create_result, 't', "Created 5GB sparse file");

my $test_4_5gb = $node->safe_psql('postgres',
	"SELECT test_sparse_write_read('$testfile', 4.5, 'TEST_DATA_AT_4.5GB');");
is($test_4_5gb, 't', "Write/read successful at 4.5GB offset");

my $verify_4_5gb = $node->safe_psql('postgres',
	"SELECT test_verify_offset_native('$testfile', 4.5, 'TEST_DATA_AT_4.5GB');");
is($verify_4_5gb, 't', "Native verification confirms data at correct 4.5GB offset");

$node->stop;

done_testing();
