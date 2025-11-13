# src/bin/pg_ctl/t/005_shared_memory_handle_inheritance.pl

# Copyright (c) 2025, PostgreSQL Global Development Group

# Test that shared memory handles are not inherited by child processes on Windows.
#
# Without the fix, child processes inherit shared memory handles from the backend,
# causing "pre-existing shared memory block is still in use" errors on restart.

use strict;
use warnings;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(sleep);
use IPC::Run qw(start);

# This test is Windows-specific
if ($^O ne 'MSWin32')
{
	plan skip_all => 'test is specific to Windows shared memory handle inheritance';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

my $marker_file = $node->data_dir . '/ps_marker.txt';
unlink $marker_file if -e $marker_file;

# Build the SQL command to run asynchronously
my $copy_cmd = qq{COPY (SELECT 1) TO PROGRAM 'powershell -Command "echo marker > $marker_file; Start-Sleep 120"'};

# Start psql asynchronously so we can kill postmaster while COPY is running
my $psql_handle = IPC::Run::start(
	[ 'psql', '-p', $node->port, '-d', 'postgres', '-c', $copy_cmd ],
	'>', \my $psql_stdout,
	'2>', \my $psql_stderr);

# Wait for the PowerShell process to start
my $ps_spawned = 0;
for (my $i = 0; $i < 100; $i++)
{
	if (-e $marker_file)
	{
		$ps_spawned = 1;
		last;
	}
	sleep 0.1;
}

ok($ps_spawned, 'child process spawned successfully');

# Get postmaster PID from the pidfile
my $pidfile = $node->data_dir . '/postmaster.pid';
open(my $pidfh, '<', $pidfile) or die "Cannot open $pidfile: $!";
my $postmaster_pid = <$pidfh>;
chomp $postmaster_pid;
close $pidfh;

ok($postmaster_pid && $postmaster_pid =~ /^\d+$/, "got postmaster PID: $postmaster_pid");

# Forcefully kill postmaster to simulate a crash
# Use taskkill /F on Windows for forceful termination
system("taskkill /F /PID $postmaster_pid >nul 2>&1");
sleep 1;

# Verify postmaster is dead
my $check_result = system("tasklist /FI \"PID eq $postmaster_pid\" 2>nul | findstr /C:\"$postmaster_pid\" >nul 2>&1");
ok($check_result != 0, 'postmaster process terminated');

# Reset node's internal state so it knows the server is stopped
# This prevents "node is already running" error
$node->{_pid} = undef;

# Try to restart PostgreSQL
# If the PowerShell process inherited the shared memory handle, this will fail
# with "pre-existing shared memory block is still in use"
my $restart_success = 0;
my $restart_error = '';
my $has_shmem_error = 0;

eval {
	$node->start;
	$restart_success = 1;
	1;
} or do {
	$restart_error = $@ || 'unknown error';
};

my $log_content = $node->log_content;
if ($log_content =~ /pre-existing shared memory block is still in use/i)
{
	$has_shmem_error = 1;
}

if ($restart_success)
{
	pass('PostgreSQL restarted successfully - shared memory handles not inherited');
	
	my $result = $node->safe_psql('postgres', 'SELECT 1');
	is($result, '1', 'server is responding to queries after restart');
	
	$node->stop;
}
else
{
	if ($has_shmem_error)
	{
		fail('PostgreSQL failed to restart - shared memory handle was inherited by child process');
		diag("Found expected error: 'pre-existing shared memory block is still in use'");
		
		my @log_lines = split /\n/, $log_content;
		my @shmem_lines = grep { /shared memory/i } @log_lines;
		if (@shmem_lines)
		{
			diag("Shared memory errors:");
			diag(join "\n", @shmem_lines[-3..-1]);
		}
	}
	else
	{
		fail('PostgreSQL failed to restart for unexpected reason (not shared memory)');
		diag("Restart error: $restart_error");
		diag("This may indicate a different problem - check the logs");
	}
}

# Cleanup
$psql_handle->kill_kill if $psql_handle;
cleanup_powershell_processes();
unlink $marker_file if -e $marker_file;

done_testing();

sub cleanup_powershell_processes
{
	system('powershell -Command "Get-Process powershell -ErrorAction SilentlyContinue | Where-Object {$_.Id -ne $PID} | Stop-Process -Force -ErrorAction SilentlyContinue" 2>nul');
}