param([ValidateRange(1,120)][int]$Minutes=5,[ValidateRange(1,60)][int]$IntervalSeconds=30)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
$base='http://127.0.0.1:8080/api/v1'
$record=Get-Content runtime/processes.json -Raw -Encoding UTF8|ConvertFrom-Json
$stamp=Get-Date -Format yyyyMMdd-HHmmss
$directory=Join-Path $root "artifacts/stability-$stamp"
New-Item -ItemType Directory -Force $directory|Out-Null
$receipt=Join-Path $directory 'processes.json'
foreach($kind in @('app','server')){
    $process=Get-Process -Id $record."${kind}_pid" -ErrorAction SilentlyContinue
    if($process -and $process.Path -eq $record."${kind}_path"){$record|Add-Member -NotePropertyName "${kind}_started_utc" -NotePropertyValue $process.StartTime.ToUniversalTime().ToString('o') -Force}
}
$record|ConvertTo-Json|Set-Content $receipt -Encoding UTF8
$failures=0
try{
$revision='unavailable'
$git=Join-Path $env:ProgramFiles 'Git/cmd/git.exe'
if(Test-Path $git){$revision=& $git -c "safe.directory=$root" rev-parse HEAD}
if($record.port){$base="http://127.0.0.1:$($record.port)/api/v1"}
@{git_revision=$revision;binary_sha256=(Get-FileHash runtime/avqm.exe -Algorithm SHA256).Hash;configuration=(Invoke-RestMethod "$base/config" -TimeoutSec 5);interval_seconds=$IntervalSeconds;expected_minutes=$Minutes}|ConvertTo-Json -Depth 12|Set-Content "$directory/metadata.json" -Encoding UTF8
$start=[DateTimeOffset]::UtcNow
$deadline=$start.AddMinutes($Minutes)
$failures=0
do {
    $time=[DateTimeOffset]::UtcNow
    try {
        $response=Invoke-WebRequest "$base/chains" -UseBasicParsing -TimeoutSec 10
        $snapshot=$response.Content|ConvertFrom-Json
        $process=Get-Process -Id $record.app_pid -ErrorAction Stop
        $server=Get-Process -Id $record.server_pid -ErrorAction Stop
        $row=@{time_ms=$time.ToUnixTimeMilliseconds();elapsed_s=($time-$start).TotalSeconds;working_set=$process.WorkingSet64;private_bytes=$process.PrivateMemorySize64;cpu_seconds=$process.CPU;handles=$process.Handles;server_working_set=$server.WorkingSet64;server_cpu_seconds=$server.CPU;logical_processors=[int]$env:NUMBER_OF_PROCESSORS;chains=@($snapshot|ForEach-Object{$_})}
        ($row|ConvertTo-Json -Depth 12 -Compress)|Add-Content "$directory/samples.jsonl" -Encoding UTF8
        $response.Content|Set-Content "$directory/latest.json" -Encoding UTF8
    } catch {
        $failures++
        @{time=$time.ToString('o');error=$_.Exception.Message}|ConvertTo-Json -Compress|Add-Content "$directory/errors.jsonl" -Encoding UTF8
    }
    if([DateTimeOffset]::UtcNow -ge $deadline){break}
    $remaining=($deadline-[DateTimeOffset]::UtcNow).TotalSeconds
    if($remaining -gt 0){Start-Sleep -Milliseconds ([int][Math]::Min($IntervalSeconds*1000,$remaining*1000))}
} while($true)
@{started=$start.ToString('o');finished=[DateTimeOffset]::UtcNow.ToString('o');minutes=$Minutes;poll_failures=$failures;note='Collection complete; inspect frame rate, metrics, reconnect epochs and memory trend before declaring pass.'}|ConvertTo-Json|Set-Content "$directory/result.json" -Encoding UTF8
Write-Output "Stability evidence: $directory"
}finally{
    $cleanup=@{time_utc=[DateTimeOffset]::UtcNow.ToString('o');receipt=$receipt}
    try{& "$PSScriptRoot/stop.ps1" -RecordPath $receipt;$cleanup.status='completed_with_ownership_checks'}
    catch{$cleanup.status='failed';$cleanup.error=$_.Exception.Message;throw}
    finally{$cleanup|ConvertTo-Json|Set-Content "$directory/cleanup.json" -Encoding UTF8}
}
if($failures){exit 1}
