# Copyright (c) 2025, PostgreSQL Global Development Group

# Test that shared memory Section handles are not inherited by child processes
# spawned via COPY TO PROGRAM on Windows.

use strict;
use warnings;
use threads;

use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils;
use Test::More;
use Time::HiRes qw(sleep);

if ($^O ne 'MSWin32')
{
	plan skip_all => 'test is specific to Windows handle inheritance';
}

my $handle_info = $ENV{HANDLE_INFO};
if (!$handle_info || !-x $handle_info)
{
	plan skip_all => 'handle_info utility not found';
}

my $node = PostgreSQL::Test::Cluster->new('main');
$node->init;
$node->start;

# Get baseline PowerShell processes
my @before = `tasklist /FI "IMAGENAME eq powershell.exe" /FO CSV /NH 2>nul`;
my %before = map { $1 => 1 } grep { /"powershell\.exe","(\d+)"/ } @before;

# Run COPY TO PROGRAM in a thread so we can check handles while it runs
my $thr = threads->create(sub {
	$node->safe_psql('postgres', q{COPY (SELECT 1) TO PROGRAM 'powershell -Command "$input | Out-File output.txt; Start-Sleep 120"';});
});

# Wait for PowerShell to spawn
my $child_pid;
for (my $i = 0; $i < 50; $i++)
{
	my @after = `tasklist /FI "IMAGENAME eq powershell.exe" /FO CSV /NH 2>nul`;
	my @new = grep { /"powershell\.exe","(\d+)"/ && !$before{$1} } @after;
	if (@new && $new[0] =~ /"powershell\.exe","(\d+)"/)
	{
		$child_pid = $1;
		last;
	}
	sleep 0.1;
}

ok($child_pid, "powershell.exe spawned (PID: $child_pid)");

# Get ALL postgres.exe processes
my @pg_list = `tasklist /FI "IMAGENAME eq postgres.exe" /FO CSV /NH 2>nul`;
my @postgres_pids = map { /"postgres\.exe","(\d+)"/ ? $1 : () } @pg_list;

diag("Found " . scalar(@postgres_pids) . " postgres.exe process(es)");

# Collect ALL Section handles from ALL postgres processes
my %all_postgres_sections = ();
my %all_shmem_handles = ();

diag("\n=== ALL SECTION HANDLES IN POSTGRES.EXE PROCESSES ===");
foreach my $pg_pid (@postgres_pids)
{
	my $pg_verbose = `"$handle_info" -v $pg_pid 2>&1`;
	
	my @pid_sections = ();
	while ($pg_verbose =~ /Section handle (0x[0-9a-f]+): (.+)/gi)
	{
		my ($handle_value, $handle_name) = ($1, $2);
		push @pid_sections, { handle => $handle_value, name => $handle_name };
		$all_postgres_sections{$handle_name} = {
			pid => $pg_pid,
			handle => $handle_value
		};
		
		# Track ALL PostgreSQL shared memory handles - both patterns
		if ($handle_name =~ /Global\/PostgreSQL[:.]/i)
		{
			$all_shmem_handles{$handle_name} = {
				pid => $pg_pid,
				handle => $handle_value
			};
		}
	}
	
	if (@pid_sections > 0)
	{
		diag("PID $pg_pid has " . scalar(@pid_sections) . " Section handle(s):");
		foreach my $s (@pid_sections)
		{
			my $marker = "";
			if ($s->{name} =~ /Global\/PostgreSQL:/i)
			{
				$marker = " [SHARED MEMORY - MAIN]";
			}
			elsif ($s->{name} =~ /Global\/PostgreSQL\.(\d+)$/i)
			{
				$marker = " [SHARED MEMORY - SEGMENT]";
			}
			diag("  $s->{handle}: $s->{name}$marker");
		}
	}
}

my $total_pg_sections = scalar(keys %all_postgres_sections);
my $shmem_count = scalar(keys %all_shmem_handles);
diag("\nTotal unique Section handles across all postgres.exe: $total_pg_sections");
diag("Of which are shared memory handles: $shmem_count");

ok($shmem_count > 0, "found $shmem_count shared memory Section handle(s) in postgres processes");

# Show ALL Section handles in ALL PowerShell processes
diag("\n=== ALL SECTION HANDLES IN POWERSHELL.EXE PROCESSES ===");
my @inherited_shmem = ();
my %all_ps_sections = ();

# Get ALL PowerShell processes
my @ps_list = `tasklist /FI "IMAGENAME eq powershell.exe" /FO CSV /NH 2>nul`;
my @ps_pids = map { /"powershell\.exe","(\d+)"/ ? $1 : () } @ps_list;

diag("Found " . scalar(@ps_pids) . " powershell.exe process(es)");

foreach my $ps_pid (@ps_pids)
{
	my $ps_verbose = `"$handle_info" -v $ps_pid 2>&1`;
	
	my @pid_sections = ();
	while ($ps_verbose =~ /Section handle (0x[0-9a-f]+): (.+)/gi)
	{
		my ($handle_value, $handle_name) = ($1, $2);
		push @pid_sections, { handle => $handle_value, name => $handle_name };
		$all_ps_sections{$handle_name} = {
			pid => $ps_pid,
			handle => $handle_value
		};
		
		# Check if this matches ANY postgres shared memory handle
		if (exists $all_shmem_handles{$handle_name})
		{
			push @inherited_shmem, {
				ps_pid => $ps_pid,
				ps_handle => $handle_value,
				pg_pid => $all_shmem_handles{$handle_name}->{pid},
				pg_handle => $all_shmem_handles{$handle_name}->{handle},
				name => $handle_name
			};
		}
	}
	
	if (@pid_sections > 0)
	{
		diag("PID $ps_pid has " . scalar(@pid_sections) . " Section handle(s):");
		foreach my $s (@pid_sections)
		{
			my $from_pg = exists($all_postgres_sections{$s->{name}}) ? " [FROM POSTGRES]" : "";
			my $marker = "";
			if ($s->{name} =~ /Global\/PostgreSQL:/i)
			{
				$marker = " [SHARED MEMORY - MAIN]";
			}
			elsif ($s->{name} =~ /Global\/PostgreSQL\.(\d+)$/i)
			{
				$marker = " [SHARED MEMORY - SEGMENT]";
			}
			diag("  $s->{handle}: $s->{name}$from_pg$marker");
		}
	}
	else
	{
		diag("PID $ps_pid has no Section handles");
	}
}

my $total_ps_sections = scalar(keys %all_ps_sections);
diag("\nTotal unique Section handles across all powershell.exe: $total_ps_sections");

# The test passes if NO shared memory handles were inherited
is(scalar(@inherited_shmem), 0, 'no shared memory Section handles inherited by child process');

if (@inherited_shmem)
{
	diag("\n=== INHERITED SHARED MEMORY HANDLES ===");
	foreach my $ih (@inherited_shmem)
	{
		diag("PowerShell PID $ih->{ps_pid} inherited from postgres PID $ih->{pg_pid}:");
		diag("  PowerShell handle: $ih->{ps_handle}");
		diag("  Postgres handle:   $ih->{pg_handle}");
		diag("  Shared memory:     $ih->{name}");
	}
}
else
{
	diag("\n=== SUCCESS ===");
	diag("PostgreSQL processes have $shmem_count shared memory handle(s).");
	diag("PowerShell processes inherited 0 shared memory handles.");
}

# Summary
diag("\n=== Test Summary ===");
diag("PostgreSQL processes checked: " . scalar(@postgres_pids));
diag("PostgreSQL total Section handles: $total_pg_sections");
diag("PostgreSQL shared memory handles: $shmem_count");
diag("PowerShell processes checked: " . scalar(@ps_pids));
diag("PowerShell total Section handles: $total_ps_sections");
diag("Shared memory handles inherited: " . scalar(@inherited_shmem));

# Cleanup
system('taskkill /F /FI "IMAGENAME eq powershell.exe" >nul 2>&1');
$thr->detach();
$node->stop;

done_testing();