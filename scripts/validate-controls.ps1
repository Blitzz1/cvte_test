param([ValidateRange(60,120)][int]$Seconds=75)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
if(Get-Process avqm,mediamtx -ErrorAction SilentlyContinue){throw 'Stop existing media processes before control verification'}
$directory=Join-Path $root ('artifacts/controls-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $directory|Out-Null
$config=Get-Content config/no-audio.json -Raw -Encoding UTF8|ConvertFrom-Json
foreach($chain in $config.chains){$chain.enabled=$false}
$config.settings.webhook=''
$profile=Join-Path $directory 'app.json'
[IO.File]::WriteAllText($profile,($config|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
$receipt=Join-Path $directory 'processes.json'
$base='http://127.0.0.1:8080/api/v1'
$watch=[Diagnostics.Stopwatch]::StartNew()
try{
    & "$PSScriptRoot/start.ps1" -Config $profile -Seconds $Seconds -NoBrowser
    Copy-Item runtime/processes.json $receipt
    $record=Get-Content $receipt -Raw -Encoding UTF8|ConvertFrom-Json
    $headers=@{'X-AVQM-Token'=(Invoke-RestMethod "$base/health").token}
    function Snapshot([string]$stage){
        if($watch.Elapsed.TotalSeconds -gt $Seconds-8){throw 'Control test deadline approaching'}
        $process=Get-Process -Id $record.app_pid -ErrorAction Stop
        if($process.PrivateMemorySize64 -gt 512MB){throw 'Control test memory guard reached'}
        $values=@(Invoke-RestMethod "$base/chains"|ForEach-Object {$_})
        [IO.File]::WriteAllText((Join-Path $directory ($stage+'.json')),($values|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
        return $values
    }
    function Command([string]$id,[string]$action){
        $job=Invoke-RestMethod "$base/chains/$id/$action" -Method Post -Headers $headers -ContentType application/json -Body '{}' -TimeoutSec 3
        for($i=0;$i -lt 30;$i++){
            $status=Invoke-RestMethod "$base/commands/$($job.command_id)" -TimeoutSec 3
            if($status.state -eq 'failed'){throw $status.error}
            if($status.state -eq 'completed'){return}
            Start-Sleep -Milliseconds 200
        }
        throw 'Control command did not complete in time'
    }
    function WaitOutput([int]$count){
        for($i=0;$i -lt 25;$i++){
            $values=Snapshot 'waiting'
            if(@($values|Where-Object output_fps -gt 20).Count -eq $count){return $values}
            Start-Sleep -Milliseconds 500
        }
        throw "Expected $count live outputs"
    }
    $initial=Snapshot 'initial'
    if(@($initial|Where-Object state -ne 'stopped').Count){throw 'Manual-start profile did not start idle'}
    Command 'rtsp-udp' 'start'
    $one=WaitOutput 1
    $one=Snapshot 'one-video-only'
    if(@($one|Where-Object {$_.id -ne 'rtsp-udp' -and $_.state -ne 'stopped'}).Count){throw 'Other chains started unexpectedly'}
    foreach($id in 'rtsp-tcp','rtmp-3','rtmp-4'){Command $id 'start'}
    $four=WaitOutput 4
    Start-Sleep -Seconds 7
    $four=Snapshot 'four-running'
    $silent=$four|Where-Object id -eq 'rtsp-udp'
    if($null -ne $silent.av_offset_ms -or $silent.sync_validity -ne 'not_applicable'){throw 'No-audio synchronization should be not applicable'}
    Command 'rtmp-3' 'stop'
    Start-Sleep -Seconds 2
    $three=Snapshot 'three-running'
    foreach($id in 'rtsp-udp','rtsp-tcp','rtmp-4'){
        $before=$four|Where-Object id -eq $id;$after=$three|Where-Object id -eq $id
        if($before.session -ne $after.session -or $after.output_fps -lt 20){throw 'Stopping one chain disrupted another'}
    }
    Command 'rtmp-3' 'start'
    $again=WaitOutput 4
    $again=Snapshot 'four-restored'
    @{passed=$true;seconds=$watch.Elapsed.TotalSeconds;source='data/media/sample-3m.mp4';video_only='data/media/sample-1080p30-no-audio.mp4';binary_sha256=(Get-FileHash runtime/avqm.exe -Algorithm SHA256).Hash;scope='Idle startup, start one, add three, stop one with session isolation, restart and no-audio applicability. Not a stability test.'}|ConvertTo-Json|Set-Content (Join-Path $directory 'result.json') -Encoding UTF8
}finally{
    if(Test-Path $receipt){& "$PSScriptRoot/stop.ps1" -RecordPath $receipt}
}
Write-Output "Control-test evidence: $directory"
