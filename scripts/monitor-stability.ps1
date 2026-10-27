param([int]$Minutes=60,[int]$IntervalSeconds=30)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
$base='http://127.0.0.1:8080/api/v1'
$record=Get-Content runtime/processes.json -Raw -Encoding UTF8|ConvertFrom-Json
$stamp=Get-Date -Format yyyyMMdd-HHmmss
$directory=Join-Path $root "artifacts/stability-$stamp"
New-Item -ItemType Directory -Force $directory|Out-Null
$start=[DateTimeOffset]::UtcNow
$deadline=$start.AddMinutes($Minutes)
$failures=0
do {
    $time=[DateTimeOffset]::UtcNow
    try {
        $response=Invoke-WebRequest "$base/chains" -UseBasicParsing -TimeoutSec 10
        $snapshot=$response.Content|ConvertFrom-Json
        $process=Get-Process -Id $record.app_pid -ErrorAction Stop
        $row=@{time_ms=$time.ToUnixTimeMilliseconds();elapsed_s=($time-$start).TotalSeconds;working_set=$process.WorkingSet64;private_bytes=$process.PrivateMemorySize64;cpu_seconds=$process.CPU;handles=$process.Handles;chains=$snapshot}
        ($row|ConvertTo-Json -Depth 12 -Compress)|Add-Content "$directory/samples.jsonl" -Encoding UTF8
        $response.Content|Set-Content "$directory/latest.json" -Encoding UTF8
    } catch {
        $failures++
        @{time=$time.ToString('o');error=$_.Exception.Message}|ConvertTo-Json -Compress|Add-Content "$directory/errors.jsonl" -Encoding UTF8
    }
    if([DateTimeOffset]::UtcNow -ge $deadline){break}
    Start-Sleep -Seconds $IntervalSeconds
} while($true)
@{started=$start.ToString('o');finished=[DateTimeOffset]::UtcNow.ToString('o');minutes=$Minutes;poll_failures=$failures;note='Collection complete; inspect frame rate, metrics, reconnect epochs and memory trend before declaring pass.'}|ConvertTo-Json|Set-Content "$directory/result.json" -Encoding UTF8
Write-Output "Stability evidence: $directory"
if($failures){exit 1}
