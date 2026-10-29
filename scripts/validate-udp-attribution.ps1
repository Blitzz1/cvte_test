param([ValidateRange(75,150)][int]$Seconds=135,[ValidateRange(1,50)][int]$PacketLossPct=10,[switch]$AttributionOnly)
$ErrorActionPreference='Stop'
if(-not $AttributionOnly -and $Seconds -lt 115){throw 'The full comparison requires at least 115 seconds; use -AttributionOnly for a short run'}
$root=Split-Path $PSScriptRoot
Set-Location $root
if(Get-Process avqm,mediamtx -ErrorAction SilentlyContinue){throw 'Stop existing media before bounded UDP validation'}
$directory=Join-Path $root ('artifacts/udp-attribution-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $directory|Out-Null
$config=Get-Content config/stability-four.json -Raw -Encoding UTF8|ConvertFrom-Json
$config.chains=@($config.chains|Where-Object id -eq 'rtsp-udp')
$config.settings.webhook=''
$profile=Join-Path $directory 'app.json'
[IO.File]::WriteAllText($profile,($config|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
$receipt=Join-Path $directory 'processes.json'
$base='http://127.0.0.1:8080/api/v1';$watch=[Diagnostics.Stopwatch]::StartNew()
$previous=$null;$high=0;$peak=0;$serverPeak=0;$maxCpu=0
try{
    & "$PSScriptRoot/start.ps1" -Config $profile -Seconds $Seconds -NoBrowser
    Copy-Item runtime/processes.json $receipt
    $record=Get-Content $receipt -Raw -Encoding UTF8|ConvertFrom-Json
    $headers=@{'X-AVQM-Token'=(Invoke-RestMethod "$base/health" -TimeoutSec 2).token}
    function Sample([string]$phase){
        if($watch.Elapsed.TotalSeconds -gt $Seconds-8){throw 'UDP validation deadline approaching'}
        $ap=Get-Process -Id $record.app_pid -ErrorAction Stop;$sp=Get-Process -Id $record.server_pid -ErrorAction Stop
        $elapsed=$watch.Elapsed.TotalSeconds
        $script:peak=[Math]::Max($script:peak,$ap.PrivateMemorySize64);$script:serverPeak=[Math]::Max($script:serverPeak,$sp.PrivateMemorySize64)
        if($ap.PrivateMemorySize64 -gt 512MB -or $sp.PrivateMemorySize64 -gt 256MB){throw 'Memory guard reached'}
        if($script:previous){$cpu=100*($ap.CPU+$sp.CPU-$script:previous.cpu)/[Math]::Max(.1,$elapsed-$script:previous.time)/[int]$env:NUMBER_OF_PROCESSORS;$script:maxCpu=[Math]::Max($script:maxCpu,$cpu);if($cpu -gt 20){$script:high++}else{$script:high=0};if($script:high -ge 2){throw 'CPU guard reached'}}
        $script:previous=@{time=$elapsed;cpu=$ap.CPU+$sp.CPU}
        $response=Invoke-RestMethod "$base/chains" -TimeoutSec 2
        $value=foreach($item in $response){if($item.id -eq 'rtsp-udp'){$item}}
        @{phase=$phase;elapsed_s=$elapsed;chain=$value}|ConvertTo-Json -Depth 14 -Compress|Add-Content (Join-Path $directory 'samples.jsonl') -Encoding UTF8
        return $value
    }
    function Phase([string]$name,[int]$duration){$timer=[Diagnostics.Stopwatch]::StartNew();while($timer.Elapsed.TotalSeconds -lt $duration){$null=Sample $name;Start-Sleep -Milliseconds 1000};return Sample $name}
    function Fault([string]$body){Invoke-RestMethod "$base/chains/rtsp-udp/fault-injection" -Method Post -Headers $headers -ContentType application/json -Body $body -TimeoutSec 2|Out-Null}
    $baseline=Phase 'baseline' 14
    # Isolate UDP attribution from a rare, already-attributed output scheduling drop.
    $baselinePresentation=if($baseline.loss_reasons.presentation_dropped){[int]$baseline.loss_reasons.presentation_dropped}else{0}
    if($baseline.output_fps -lt 20 -or $baseline.loss_unknown -ne 0 -or
       $baseline.loss_missing-$baselinePresentation -ne 0 -or $baseline.loss_pct -gt 1){throw 'UDP baseline contains unexplained or transport loss'}
    Fault ('{"packet_loss_pct":'+$PacketLossPct+',"seed":1}')
    $partial=Phase ('udp'+$PacketLossPct) 24
    $blocked=$null;$presentation=$null
    if(-not $AttributionOnly){
        Fault '{"packet_loss_pct":100,"seed":1}'
        $blocked=Phase 'udp100' 8
    }
    Fault '{}'
    $recovered=Phase 'recovery' 18
    if(-not $AttributionOnly){
        Fault '{"presentation_drop_pct":30,"seed":1}'
        $presentation=Phase 'presentation30' 18
        if($presentation.loss_reasons.presentation_dropped -le 0){throw 'Presentation comparison missing'}
        Fault '{}'
        $final=Phase 'final' 17
    }else{$final=$recovered}
    if($final.output_fps -lt 20 -or $final.loss_missing -ne 0){throw 'UDP final recovery failed'}
    @{passed=$true;packet_loss_pct=$PacketLossPct;seconds=$watch.Elapsed.TotalSeconds;peak_private_mib=$peak/1MB;server_peak_mib=$serverPeak/1MB;max_cpu_pct=$maxCpu;binary_sha256=(Get-FileHash runtime/avqm.exe -Algorithm SHA256).Hash;baseline=$baseline;partial=$partial;blocked=$blocked;recovery=$recovered;presentation=$presentation;final=$final}|ConvertTo-Json -Depth 14|Set-Content (Join-Path $directory 'result.json') -Encoding UTF8
}finally{if(Test-Path $receipt){& "$PSScriptRoot/stop.ps1" -RecordPath $receipt}}
Write-Output "UDP attribution evidence: $directory"
