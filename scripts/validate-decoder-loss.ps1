param([ValidateSet('rtmp-4','rtsp-tcp')][string]$Chain='rtmp-4',[ValidateRange(90,120)][int]$Seconds=110)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
if(Get-Process avqm,mediamtx -ErrorAction SilentlyContinue){throw 'Stop existing media instances before bounded decoder validation'}
$directory=Join-Path $root ('artifacts/decoder-loss-'+$Chain+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $directory|Out-Null
$config=Get-Content config/stability-four.json -Raw -Encoding UTF8|ConvertFrom-Json
foreach($c in $config.chains){$c.enabled=$c.id -eq $Chain}
$config.settings.webhook=''
$profile=Join-Path $directory 'app.json'
[IO.File]::WriteAllText($profile,($config|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
$receipt=Join-Path $directory 'processes.json'
$base='http://127.0.0.1:8080/api/v1'
$watch=[Diagnostics.Stopwatch]::StartNew()
$peak=0;$serverPeak=0;$maxCpu=0;$previous=$null;$highCpu=0
try{
    & "$PSScriptRoot/start.ps1" -Config $profile -Seconds $Seconds -NoBrowser
    Copy-Item runtime/processes.json $receipt
    $record=Get-Content $receipt -Raw -Encoding UTF8|ConvertFrom-Json
    $headers=@{'X-AVQM-Token'=(Invoke-RestMethod "$base/health" -TimeoutSec 2).token}
    function Sample([string]$phase){
        if($watch.Elapsed.TotalSeconds -gt $Seconds-8){throw 'Decoder validation deadline approaching'}
        $ap=Get-Process -Id $record.app_pid -ErrorAction Stop
        $sp=Get-Process -Id $record.server_pid -ErrorAction Stop
        $script:peak=[Math]::Max($script:peak,$ap.PrivateMemorySize64)
        $script:serverPeak=[Math]::Max($script:serverPeak,$sp.PrivateMemorySize64)
        if($ap.PrivateMemorySize64 -gt 512MB -or $sp.PrivateMemorySize64 -gt 256MB){throw 'Memory guard reached'}
        $elapsed=$watch.Elapsed.TotalSeconds
        if($script:previous){
            $cpu=100*($ap.CPU+$sp.CPU-$script:previous.cpu)/[Math]::Max(.1,$elapsed-$script:previous.time)/[int]$env:NUMBER_OF_PROCESSORS
            $script:maxCpu=[Math]::Max($script:maxCpu,$cpu)
            if($cpu -gt 20){$script:highCpu++}else{$script:highCpu=0}
            if($script:highCpu -ge 2){throw 'CPU guard reached'}
        }
        $script:previous=@{time=$elapsed;cpu=$ap.CPU+$sp.CPU}
        $all=Invoke-RestMethod "$base/chains" -TimeoutSec 2
        $value=@($all)|Where-Object id -eq $Chain
        if(-not $value){throw 'Target chain missing from HTTP snapshot'}
        @{phase=$phase;elapsed_s=$elapsed;chain=$value}|ConvertTo-Json -Depth 12 -Compress|Add-Content (Join-Path $directory 'samples.jsonl') -Encoding UTF8
        return $value
    }
    function Phase([string]$name,[int]$duration){
        $timer=[Diagnostics.Stopwatch]::StartNew()
        while($timer.Elapsed.TotalSeconds -lt $duration){$null=Sample $name;Start-Sleep -Milliseconds 1000}
        return Sample $name
    }
    function Fault([string]$body){Invoke-RestMethod "$base/chains/$Chain/fault-injection" -Method Post -Headers $headers -ContentType application/json -Body $body -TimeoutSec 2|Out-Null}
    $baseline=Phase 'baseline' 14
    if($baseline.output_fps -lt 20 -or $baseline.loss_missing -ne 0){throw 'Baseline did not reach clean normal playback'}
    Fault '{"frame_loss_pct":30,"seed":1}'
    $receive=Phase 'receiver30' 20
    if($receive.loss_reasons.receiver_discarded -le 0){throw 'Receiver discard evidence absent'}
    if($receive.loss_unknown -ne 0){throw 'Receiver loss still contains unresolved source frames'}
    Fault '{}'
    $recovered=Phase 'recovery' 18
    if($recovered.output_fps -lt 20 -or $recovered.loss_missing -ne 0){throw 'Receiver fault did not recover after mature window'}
    Fault '{"presentation_drop_pct":30,"seed":1}'
    $presentation=Phase 'presentation30' 18
    if($presentation.loss_reasons.presentation_dropped -le 0 -or $presentation.loss_unknown -ne 0){throw 'Presentation comparison did not resolve'}
    if($presentation.loss_reasons.decode_after_input_loss -gt 0 -or $presentation.loss_reasons.decode_no_output -gt 0){throw 'Presentation discard was misattributed to decoder'}
    Fault '{}'
    $final=Phase 'final' 17
    if($final.output_fps -lt 20 -or $final.loss_missing -ne 0){throw 'Final recovery failed'}
    @{passed=$true;chain=$Chain;seconds=$watch.Elapsed.TotalSeconds;peak_private_mib=$peak/1MB;server_peak_mib=$serverPeak/1MB;max_combined_cpu_pct=$maxCpu;binary_sha256=(Get-FileHash runtime/avqm.exe -Algorithm SHA256).Hash;baseline=$baseline;receiver30=$receive;recovery=$recovered;presentation30=$presentation;final=$final}|ConvertTo-Json -Depth 14|Set-Content (Join-Path $directory 'result.json') -Encoding UTF8
}finally{
    if(Test-Path $receipt){& "$PSScriptRoot/stop.ps1" -RecordPath $receipt}
}
Write-Output "Decoder attribution evidence: $directory"
