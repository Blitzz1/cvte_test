param([ValidateRange(75,120)][int]$Seconds=90)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
if(Get-Process avqm,mediamtx -ErrorAction SilentlyContinue){throw 'Stop existing media instances before preview validation'}
$directory=Join-Path $root ('artifacts/preview-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $directory|Out-Null
$config=Get-Content config/stability-four.json -Raw -Encoding UTF8|ConvertFrom-Json
$config.chains[0].source='data/media/sample-1080p30-no-audio.mp4'
$config.chains[0]|Add-Member -NotePropertyName has_audio -NotePropertyValue $false -Force
$config.settings.webhook=''
$profile=Join-Path $directory 'app.json'
[IO.File]::WriteAllText($profile,($config|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
$receipt=Join-Path $directory 'processes.json'
$base='http://127.0.0.1:8080/api/v1'
$watch=[Diagnostics.Stopwatch]::StartNew();$peak=0;$serverPeak=0;$maxCpu=0;$previous=$null;$highCpu=0
try{
    & "$PSScriptRoot/start.ps1" -Config $profile -Seconds $Seconds -NoBrowser
    Copy-Item runtime/processes.json $receipt
    $record=Get-Content $receipt -Raw -Encoding UTF8|ConvertFrom-Json
    $headers=@{'X-AVQM-Token'=(Invoke-RestMethod "$base/health" -TimeoutSec 2).token}
    function CheckGuard {
        if($watch.Elapsed.TotalSeconds -gt $Seconds-8){throw 'Preview test deadline approaching'}
        $appProcess=Get-Process -Id $record.app_pid -ErrorAction Stop
        $serverProcess=Get-Process -Id $record.server_pid -ErrorAction Stop
        $script:peak=[Math]::Max($script:peak,$appProcess.PrivateMemorySize64)
        $script:serverPeak=[Math]::Max($script:serverPeak,$serverProcess.PrivateMemorySize64)
        if($appProcess.PrivateMemorySize64 -gt 512MB -or $serverProcess.PrivateMemorySize64 -gt 256MB){throw 'Preview memory guard reached'}
        $elapsed=$watch.Elapsed.TotalSeconds
        if($script:previous -and $elapsed-$script:previous.time -ge 1){
            $cpu=100*($appProcess.CPU+$serverProcess.CPU-$script:previous.cpu)/($elapsed-$script:previous.time)/[int]$env:NUMBER_OF_PROCESSORS
            $script:maxCpu=[Math]::Max($script:maxCpu,$cpu)
            if($cpu -gt 20){$script:highCpu++}else{$script:highCpu=0}
            if($script:highCpu -ge 2){throw 'Preview CPU guard reached'}
            $script:previous=@{time=$elapsed;cpu=$appProcess.CPU+$serverProcess.CPU}
        }elseif(-not $script:previous){$script:previous=@{time=$elapsed;cpu=$appProcess.CPU+$serverProcess.CPU}}
    }
    function Snapshot([string]$stage){CheckGuard;$values=@(Invoke-RestMethod "$base/chains" -TimeoutSec 2|ForEach-Object {$_});$values|ConvertTo-Json -Depth 12|Set-Content (Join-Path $directory ($stage+'.json')) -Encoding UTF8;return $values}
    function Frame([string]$id,[string]$stage){
        CheckGuard
        $response=Invoke-WebRequest "$base/chains/$id/preview.jpg" -UseBasicParsing -TimeoutSec 3
        $row=@{stage=$stage;id=$id;status=[int]$response.StatusCode;sequence=$response.Headers['X-Preview-Sequence'];frame_id=$response.Headers['X-Preview-Frame-Id'];generation=$response.Headers['X-Preview-Generation'];presented_us=$response.Headers['X-Preview-Presented-Us'];age_ms=$response.Headers['X-Preview-Age-Ms'];elapsed=$watch.Elapsed.TotalSeconds}
        $row|ConvertTo-Json -Compress|Add-Content (Join-Path $directory 'preview-frames.jsonl') -Encoding UTF8
        if($response.StatusCode -eq 200){
            if($response.Headers['Content-Type'] -notmatch 'image/jpeg'){throw 'Expected real JPEG'}
            if($stage -eq 'baseline' -and -not(Test-Path (Join-Path $directory 'actual-output.jpg'))){[IO.File]::WriteAllBytes((Join-Path $directory 'actual-output.jpg'),$response.Content)}
        }
        return $row
    }
    function Command([string]$action){
        $job=Invoke-RestMethod "$base/chains/rtsp-tcp/$action" -Method Post -Headers $headers -ContentType application/json -Body '{}' -TimeoutSec 2
        for($i=0;$i -lt 30;$i++){$state=Invoke-RestMethod "$base/commands/$($job.command_id)" -TimeoutSec 2;if($state.state -eq 'completed'){return};if($state.state -eq 'failed'){throw $state.error};Start-Sleep -Milliseconds 200}
        throw 'Lifecycle command timed out'
    }
    function Fault([string]$body){Invoke-RestMethod "$base/chains/rtsp-tcp/fault-injection" -Method Post -Headers $headers -ContentType application/json -Body $body -TimeoutSec 2|Out-Null}
    $ready=$null
    for($i=0;$i -lt 40;$i++){$ready=Snapshot 'warming';if(@($ready|Where-Object output_fps -gt 20).Count -eq 4){break};Start-Sleep -Milliseconds 300}
    if(@($ready|Where-Object output_fps -gt 20).Count -ne 4){throw 'Four live outputs did not warm up'}
    for($i=0;$i -lt 15;$i++){$baseline=Frame 'rtsp-tcp' 'baseline';Start-Sleep -Milliseconds 150}
    if($baseline.status -ne 200 -or -not $baseline.frame_id){throw 'No actual output preview frame'}
    foreach($id in 'rtsp-udp','rtmp-3','rtmp-4'){$null=Frame $id 'mosaic';Start-Sleep -Milliseconds 200;$null=Frame $id 'mosaic'}
    Fault '{"presentation_drop_pct":100}'
    $dropWatch=[Diagnostics.Stopwatch]::StartNew();$frozenSequence=$null;$frozenCount=0
    while($dropWatch.Elapsed.TotalSeconds -lt 18){
        $frame=Frame 'rtsp-tcp' 'drop100'
        if($dropWatch.Elapsed.TotalSeconds -gt 2){if($frozenSequence -and $frame.sequence -ne $frozenSequence){throw 'Preview advanced while all presentation was discarded'};$frozenSequence=$frame.sequence;$frozenCount++}
        Start-Sleep -Milliseconds 150
    }
    $drop=Snapshot 'drop100';$target=$drop|Where-Object id -eq 'rtsp-tcp'
    if($target.output_fps -gt 1 -or $target.loss_confirmed_pct -lt 99){throw 'Did not observe 100% presentation loss in mature window'}
    if(@($drop|Where-Object {$_.id -ne 'rtsp-tcp' -and $_.output_fps -lt 20}).Count){throw 'Other chains were disrupted'}
    Fault '{}'
    $recovered=$false
    for($i=0;$i -lt 25;$i++){$frame=Frame 'rtsp-tcp' 'recovery';if($frame.status -eq 200 -and $frame.sequence -ne $frozenSequence){$recovered=$true;break};Start-Sleep -Milliseconds 200}
    if(-not $recovered){throw 'Preview did not recover'}
    $oldGeneration=$frame.generation
    Command 'stop'
    $stopped=Frame 'rtsp-tcp' 'stopped'
    if($stopped.status -ne 204){throw 'Stopped chain retained old preview'}
    Command 'start'
    $restarted=$false
    for($i=0;$i -lt 40;$i++){$frame=Frame 'rtsp-tcp' 'restart';if($frame.status -eq 200 -and $frame.generation -ne $oldGeneration){$restarted=$true;break};Start-Sleep -Milliseconds 250}
    if(-not $restarted){throw 'New generation preview not observed'}
    $final=Snapshot 'final'
    $hls=Invoke-RestMethod http://127.0.0.1:9997/v3/hlsmuxers/list -TimeoutSec 2
    if(@($hls.items).Count){throw 'Main preview unexpectedly started HLS muxers'}
    @{passed=$true;seconds=$watch.Elapsed.TotalSeconds;frozen_observations=$frozenCount;peak_private_mib=$script:peak/1MB;server_peak_mib=$script:serverPeak/1MB;max_combined_cpu_pct=$script:maxCpu;hls_muxers=@($hls.items).Count;binary_sha256=(Get-FileHash runtime/avqm.exe -Algorithm SHA256).Hash;scope='Bounded four-chain actual WIC preview, 100% presentation discard freeze, recovery, stop/restart and no HLS. Not a one-hour stability test.'}|ConvertTo-Json|Set-Content (Join-Path $directory 'result.json') -Encoding UTF8
}finally{
    if(Test-Path $receipt){& "$PSScriptRoot/stop.ps1" -RecordPath $receipt}
}
Write-Output "Preview verification evidence: $directory"
