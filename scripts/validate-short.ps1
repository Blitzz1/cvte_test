param([ValidateSet(1,4)][int]$Chains=1,[ValidateRange(20,90)][int]$Seconds=30,[ValidateSet('baseline','transitions','budget','receive_loss')][string]$Scenario='baseline')
$ErrorActionPreference='Stop'
if($Scenario -eq 'transitions' -and ($Chains -ne 4 -or $Seconds -lt 75)){throw 'Transitions require four chains and 75..90 seconds'}
if($Scenario -eq 'budget' -and ($Chains -ne 1 -or $Seconds -lt 30)){throw 'Budget test requires one chain and at least 30 seconds'}
$root=Split-Path $PSScriptRoot
Set-Location $root
if(Get-Process avqm -ErrorAction SilentlyContinue){throw 'A monitor is already running; do not overlap media tests'}
$directory=Join-Path $root ('artifacts/short-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+$Scenario)
New-Item -ItemType Directory -Force $directory|Out-Null
$config=Get-Content config/app.json -Raw -Encoding UTF8|ConvertFrom-Json
if($Chains -eq 1){$config.chains=@($config.chains[1])}
$config.settings.webhook=''
if($Scenario -eq 'budget'){$config.settings|Add-Member -NotePropertyName video_queue_mib -NotePropertyValue 1 -Force;$config.settings|Add-Member -NotePropertyName video_total_mib -NotePropertyValue 8 -Force}
$config|Add-Member -NotePropertyName show_monitor -NotePropertyValue $false -Force
$profile=Join-Path $directory 'app.json'
[IO.File]::WriteAllText($profile,($config|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
$receipt=Join-Path $directory 'processes.json'
$watch=[Diagnostics.Stopwatch]::StartNew()
$base='http://127.0.0.1:8080/api/v1'
$previous=$null;$stage='';$highCpu=0;$guard=$false
try{
    & "$PSScriptRoot/start.ps1" -Config $profile -Seconds $Seconds -NoBrowser
    Copy-Item runtime/processes.json $receipt
    $record=Get-Content $receipt -Raw -Encoding UTF8|ConvertFrom-Json
    $headers=@{'X-AVQM-Token'=(Invoke-RestMethod "$base/health" -TimeoutSec 3).token}
    function Fault([string]$id,[string]$body){Invoke-RestMethod "$base/chains/$id/fault-injection" -Method Post -Headers $headers -ContentType application/json -Body $body -TimeoutSec 3|Out-Null}
    while($watch.Elapsed.TotalSeconds -lt $Seconds-5){
        $elapsed=$watch.Elapsed.TotalSeconds
        $next='baseline'
        if($Scenario -eq 'budget'){$next=if($elapsed -lt 6){'baseline'}elseif($elapsed -lt 14){'budget_delay'}else{'recovery'}}
        if($Scenario -eq 'receive_loss'){$next=if($elapsed -lt 6){'baseline'}elseif($elapsed -lt 10){'receive_loss'}else{'recovery'}}
        if($Scenario -eq 'transitions'){$next=if($elapsed -lt 8){'baseline'}elseif($elapsed -lt 16){'audio_delay'}elseif($elapsed -lt 24){'audio_recovery'}elseif($elapsed -lt 32){'video_delay'}elseif($elapsed -lt 40){'video_recovery'}elseif($elapsed -lt 52){'reconnect'}elseif($elapsed -lt 54){'udp_packet_loss'}else{'final_recovery'}}
        if($next -ne $stage){
            $target=if($Chains -eq 1){$config.chains[0].id}else{'rtmp-3'}
            if($stage -in @('audio_delay','video_delay','budget_delay','receive_loss')){Fault $target '{}'}
            if($stage -eq 'udp_packet_loss'){Fault 'rtsp-udp' '{}'}
            if($next -eq 'audio_delay'){Fault $target '{"audio_delay_ms":350}'}
            if($next -eq 'receive_loss'){Fault $target '{"frame_loss_pct":100}'}
            if($next -eq 'video_delay' -or $next -eq 'budget_delay'){Fault $target '{"video_delay_ms":800}'}
            if($next -eq 'reconnect'){Invoke-RestMethod "$base/chains/$target/restart" -Method Post -Headers $headers -ContentType application/json -Body '{}' -TimeoutSec 3|Out-Null}
            if($next -eq 'udp_packet_loss'){Fault 'rtsp-udp' '{"packet_loss_pct":100,"seed":17}'}
            $stage=$next
        }
        $response=Invoke-WebRequest "$base/chains" -UseBasicParsing -TimeoutSec 3
        $response.Content|Set-Content (Join-Path $directory ($stage+'.json')) -Encoding UTF8
        $snapshot=$response.Content|ConvertFrom-Json
        $process=Get-Process -Id $record.app_pid -ErrorAction Stop
        $cpu=if($previous){100*($process.CPU-$previous.cpu)/[Math]::Max(0.1,$elapsed-$previous.elapsed)/[int]$env:NUMBER_OF_PROCESSORS}else{0}
        $row=@{elapsed_s=$elapsed;stage=$stage;private_bytes=$process.PrivateMemorySize64;working_set=$process.WorkingSet64;cpu_pct=$cpu;chains=@($snapshot|ForEach-Object{$_})}
        $row|ConvertTo-Json -Depth 12 -Compress|Add-Content (Join-Path $directory 'samples.jsonl') -Encoding UTF8
        $previous=@{cpu=$process.CPU;elapsed=$elapsed}
        $limit=if($Chains -eq 1){512MB}else{768MB}
        $cpuLimit=if($Chains -eq 1){15}else{25}
        if($cpu -gt $cpuLimit){$highCpu++}else{$highCpu=0}
        if($process.PrivateMemorySize64 -gt $limit -or $highCpu -ge 2){$guard=$true;Write-Warning 'Resource guard reached; stopping this short test.';break}
        Start-Sleep -Milliseconds 1000
    }
    (Invoke-WebRequest "$base/events" -UseBasicParsing -TimeoutSec 3).Content|Set-Content (Join-Path $directory 'events.json') -Encoding UTF8
    @{collected=$true;scenario=$Scenario;chains=$Chains;seconds=$watch.Elapsed.TotalSeconds;resource_guard=$guard;binary_sha256=(Get-FileHash runtime/avqm.exe -Algorithm SHA256).Hash;note='Short regression only; review observations, not a stability acceptance.'}|ConvertTo-Json|Set-Content (Join-Path $directory 'result.json') -Encoding UTF8
}finally{
    if(Test-Path $receipt){& "$PSScriptRoot/stop.ps1" -RecordPath $receipt}
}
Write-Output "Short-test evidence: $directory"
if($guard){exit 1}
