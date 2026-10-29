param([ValidateRange(100,150)][int]$Seconds=120)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
if(Get-Process avqm,mediamtx -ErrorAction SilentlyContinue){throw 'Do not overlap media validation'}
$directory=Join-Path $root ('artifacts/chain-pages-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $directory|Out-Null
$config=Get-Content config/stability-four.json -Raw -Encoding UTF8|ConvertFrom-Json
$config.chains=@($config.chains|Where-Object id -eq 'rtsp-tcp')
$config.settings.webhook=''
$profile=Join-Path $directory 'app.json'
[IO.File]::WriteAllText($profile,($config|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
$receipt=Join-Path $directory 'processes.json'
$base='http://127.0.0.1:8080/api/v1';$watch=[Diagnostics.Stopwatch]::StartNew()
$peak=0;$serverPeak=0;$previous=$null;$high=0;$maxCpu=0
try{
    & "$PSScriptRoot/start.ps1" -Config $profile -Seconds $Seconds -NoBrowser
    Copy-Item runtime/processes.json $receipt
    $record=Get-Content $receipt -Raw -Encoding UTF8|ConvertFrom-Json
    $headers=@{'X-AVQM-Token'=(Invoke-RestMethod "$base/health" -TimeoutSec 2).token}
    function Samples([string]$phase){
        if($watch.Elapsed.TotalSeconds -gt $Seconds-8){throw 'Deadline approaching'}
        $ap=Get-Process -Id $record.app_pid -ErrorAction Stop;$sp=Get-Process -Id $record.server_pid -ErrorAction Stop
        $script:peak=[Math]::Max($script:peak,$ap.PrivateMemorySize64);$script:serverPeak=[Math]::Max($script:serverPeak,$sp.PrivateMemorySize64)
        if($ap.PrivateMemorySize64 -gt 512MB -or $sp.PrivateMemorySize64 -gt 256MB){throw 'Memory guard reached'}
        $elapsed=$watch.Elapsed.TotalSeconds
        if($script:previous){$cpu=100*($ap.CPU+$sp.CPU-$script:previous.cpu)/[Math]::Max(.1,$elapsed-$script:previous.time)/[int]$env:NUMBER_OF_PROCESSORS;$script:maxCpu=[Math]::Max($script:maxCpu,$cpu);if($cpu -gt 20){$script:high++}else{$script:high=0};if($script:high -ge 2){throw 'CPU guard reached'}}
        $script:previous=@{time=$elapsed;cpu=$ap.CPU+$sp.CPU}
        $all=Invoke-RestMethod "$base/chains" -TimeoutSec 2
        @{phase=$phase;elapsed_s=$elapsed;chains=@($all)}|ConvertTo-Json -Depth 14 -Compress|Add-Content (Join-Path $directory 'samples.jsonl') -Encoding UTF8
        return @($all)
    }
    function Phase([string]$phase,[int]$duration){$timer=[Diagnostics.Stopwatch]::StartNew();while($timer.Elapsed.TotalSeconds -lt $duration){$null=Samples $phase;Start-Sleep -Milliseconds 1000};return Samples $phase}
    $baseline=@(Phase 'baseline' 15)[0]
    if($baseline.pipeline_sample_count -lt 5 -or $baseline.transport_ms -eq $null){throw 'Missing real stage timestamps'}
    $session=$baseline.session
    Invoke-RestMethod "$base/chains/rtsp-tcp/fault-injection" -Method Post -Headers $headers -ContentType application/json -Body '{"video_delay_ms":800}' -TimeoutSec 2|Out-Null
    $delayed=@(Phase 'video800' 12)[0]
    if($delayed.post_decode_ms-$baseline.post_decode_ms -lt 500 -or $delayed.latency_ms-$baseline.latency_ms -lt 500){throw 'Real presentation delay was not measured'}
    if([Math]::Abs($delayed.transport_ms-$baseline.transport_ms) -gt 250){throw 'Transport incorrectly includes presentation delay'}
    Invoke-RestMethod "$base/chains/rtsp-tcp/fault-injection" -Method Post -Headers $headers -ContentType application/json -Body '{}' -TimeoutSec 2|Out-Null
    $null=Phase 'recovery' 5
    $invalid=$false
    try{Invoke-RestMethod "$base/chains" -Method Post -Headers $headers -ContentType application/json -Body '{"protocol":"unsupported"}' -TimeoutSec 2|Out-Null}catch{$invalid=$true}
    if(-not $invalid){throw 'Invalid protocol was accepted'}
    $rtmp=Invoke-RestMethod "$base/chains" -Method Post -Headers $headers -ContentType application/json -Body '{"alias":"RTMP new page","protocol":"rtmp","has_audio":true}' -TimeoutSec 2
    $two=@(Phase 'add-rtmp' 12)
    if($two.Count -ne 2 -or @($two|Where-Object {$_.output_fps -gt 20 -and $_.pipeline_sample_count -gt 0}).Count -ne 2){throw 'Runtime chain addition failed'}
    if(($two|Where-Object id -eq 'rtsp-tcp').session -ne $session){throw 'Existing chain restarted during addition'}
    $udp=Invoke-RestMethod "$base/chains" -Method Post -Headers $headers -ContentType application/json -Body '{"alias":"UDP new page","protocol":"rtsp","transport":"udp","has_audio":false,"source":"data/media/sample-1080p30-no-audio.mp4"}' -TimeoutSec 2
    $three=@(Phase 'add-udp' 15)
    if($three.Count -ne 3 -or @($three|Where-Object output_fps -gt 20).Count -ne 3){throw 'UDP page did not join independent processing'}
    if(($three|Where-Object id -eq $udp.id).sync_validity -ne 'not_applicable'){throw 'No-audio page did not preserve audio scope'}
    $saved=Get-Content $profile -Raw -Encoding UTF8|ConvertFrom-Json
    if($saved.chains.Count -ne 3 -or $rtmp.id -eq $udp.id){throw 'Created pages were not persisted with unique IDs'}
    $stop=Invoke-RestMethod "$base/chains/$($rtmp.id)/stop" -Method Post -Headers $headers -ContentType application/json -Body '{}' -TimeoutSec 2
    $final=@(Phase 'stop-added' 4)
    if(($final|Where-Object id -eq $rtmp.id).state -ne 'stopped' -or ($final|Where-Object id -eq 'rtsp-tcp').session -ne $session){throw 'Stopping added chain affected existing chain'}
    @{passed=$true;seconds=$watch.Elapsed.TotalSeconds;peak_private_mib=$peak/1MB;server_peak_mib=$serverPeak/1MB;max_cpu_pct=$maxCpu;binary_sha256=(Get-FileHash runtime/avqm.exe -Algorithm SHA256).Hash;baseline=$baseline;delayed=$delayed;final=$final;created=@($rtmp.id,$udp.id);invalid_protocol_rejected=$invalid}|ConvertTo-Json -Depth 14|Set-Content (Join-Path $directory 'result.json') -Encoding UTF8
}finally{if(Test-Path $receipt){& "$PSScriptRoot/stop.ps1" -RecordPath $receipt}}
Write-Output "Chain pages evidence: $directory"
