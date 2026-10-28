$ErrorActionPreference='Stop'
. "$PSScriptRoot/../scripts/stability-guards.ps1"
function Check($Condition,[string]$Message){if(-not $Condition){throw $Message}}
$first=@{elapsed_s=0;private_bytes=100MB;server_private_bytes=20MB;cpu_seconds=0;server_cpu_seconds=0;logical_processors=20}
$next=@{elapsed_s=10;private_bytes=110MB;server_private_bytes=21MB;cpu_seconds=50;server_cpu_seconds=1;logical_processors=20}
$guard=Test-StabilityResource $next $first
Check (-not $guard.stop -and [Math]::Abs($guard.combined_cpu_pct-25.5) -lt .001) 'first high CPU interval is recorded'
$third=@{elapsed_s=20;private_bytes=110MB;server_private_bytes=21MB;cpu_seconds=100;server_cpu_seconds=2;logical_processors=20}
$guard=Test-StabilityResource $third $next $guard.high_cpu_samples
Check ($guard.stop -and $guard.reason -eq 'sustained_combined_cpu_limit') 'two high CPU intervals stop the run'
$third.cpu_seconds=51;$third.server_cpu_seconds=1
$guard=Test-StabilityResource $third $next 1
Check (-not $guard.stop -and $guard.high_cpu_samples -eq 0) 'normal CPU resets high-CPU counter'
$third.private_bytes=513MB
Check ((Test-StabilityResource $third $next).reason -eq 'app_private_memory_limit') 'app memory limit stops immediately'
$third.private_bytes=100MB;$third.server_private_bytes=257MB
Check ((Test-StabilityResource $third $next).reason -eq 'server_private_memory_limit') 'server memory is guarded too'
Write-Output 'Stability resource guard tests passed (no media processes started).'
