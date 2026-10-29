param([ValidateRange(120,180)][int]$Seconds=150)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
if(Get-Process avqm,mediamtx -ErrorAction SilentlyContinue){throw 'Do not overlap media runs'}
$directory=Join-Path $root ('artifacts/close-pages-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $directory|Out-Null
$config=Get-Content config/stability-four.json -Raw -Encoding UTF8|ConvertFrom-Json
$config.chains=@($config.chains|Where-Object id -eq 'rtsp-tcp');$config.settings.webhook=''
$profile=Join-Path $directory 'app.json'
[IO.File]::WriteAllText($profile,($config|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
$base='http://127.0.0.1:8080/api/v1';$watch=[Diagnostics.Stopwatch]::StartNew()
$peak=0;$serverPeak=0;$previous=$null;$maxCpu=0;$high=0;$receipt=$null;$run=0
try{
    function Boot {
        $script:run++
        & "$PSScriptRoot/start.ps1" -Config $profile -Seconds $Seconds -NoBrowser
        $script:receipt=Join-Path $directory ('processes-'+$script:run+'.json')
        Copy-Item runtime/processes.json $script:receipt
        $script:record=Get-Content $script:receipt -Raw -Encoding UTF8|ConvertFrom-Json
        $script:headers=@{'X-AVQM-Token'=(Invoke-RestMethod "$base/health" -TimeoutSec 2).token}
        $script:previous=$null
    }
    function Samples([string]$phase){
        if($watch.Elapsed.TotalSeconds -gt $Seconds-10){throw 'Deadline approaching'}
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
    function Phase([string]$name,[int]$duration){$timer=[Diagnostics.Stopwatch]::StartNew();while($timer.Elapsed.TotalSeconds -lt $duration){$null=Samples $name;Start-Sleep -Milliseconds 1000};return Samples $name}
    function Close([string]$id){
        $job=Invoke-RestMethod "$base/chains/$id/close" -Method Post -Headers $headers -ContentType application/json -Body '{}' -TimeoutSec 2
        for($i=0;$i -lt 100;$i++){
            if($watch.Elapsed.TotalSeconds -gt $Seconds-10){throw 'Close command deadline approaching'}
            $state=Invoke-RestMethod "$base/commands/$($job.command_id)" -TimeoutSec 2
            if($state.state -eq 'completed'){return}
            if($state.state -eq 'failed'){throw $state.error}
            Start-Sleep -Milliseconds 200
        }
        throw 'Close did not finish'
    }
    Boot
    $a=@(Phase 'baseline' 10)[0]
    $b=Invoke-RestMethod "$base/chains" -Method Post -Headers $headers -ContentType application/json -Body '{"protocol":"rtmp","alias":"Middle RTMP"}' -TimeoutSec 2
    $null=Phase 'added-middle' 10
    $c=Invoke-RestMethod "$base/chains" -Method Post -Headers $headers -ContentType application/json -Body '{"protocol":"rtsp","transport":"udp","alias":"Surviving UDP"}' -TimeoutSec 2
    $before=@(Phase 'three-live' 12)
    if($before.Count -ne 3 -or @($before|Where-Object output_fps -gt 20).Count -ne 3){throw 'Three chains did not warm up'}
    $survivor=$before|Where-Object id -eq $c.id
    $closed=$before|Where-Object id -eq $b.id
    $null=Invoke-RestMethod "$base/audio" -Method Post -Headers $headers -ContentType application/json -Body (@{chain_id=$c.id}|ConvertTo-Json) -TimeoutSec 2
    $from=[DateTimeOffset]::UtcNow.AddMinutes(-2).ToUnixTimeMilliseconds();$to=[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    $historyUrl="$base/chains/$($b.id)/metrics?from=$from&to=$to"
    $historyBefore=@(Invoke-RestMethod $historyUrl -TimeoutSec 2|ForEach-Object {$_})
    Close $b.id
    $middleClosed=@(Phase 'closed-middle' 6)
    $audio=Invoke-RestMethod "$base/audio" -TimeoutSec 2
    if($middleClosed.Count -ne 2 -or ($middleClosed|Where-Object id -eq 'rtsp-tcp').session -ne $a.session -or ($middleClosed|Where-Object id -eq $c.id).session -ne $survivor.session){throw 'Closing middle restarted siblings'}
    if($audio.chain_id -ne $c.id -or @($middleClosed|Where-Object {$_.output_fps -gt 20 -and $_.audio_validity -eq 'device_clock'}).Count -ne 2){throw 'Surviving audio device/selection changed'}
    $historyAfter=@(Invoke-RestMethod $historyUrl -TimeoutSec 2|ForEach-Object {$_})
    if($historyBefore.Count -eq 0 -or $historyAfter.Count -ne $historyBefore.Count){throw 'Closed history was not preserved/queryable'}
    Close 'rtsp-tcp'
    $one=@(Phase 'closed-first' 4)
    if($one.Count -ne 1 -or $one[0].session -ne $survivor.session -or $one[0].output_fps -lt 20){throw 'Closing first affected survivor'}
    Close $c.id
    $empty=@(Samples 'empty')
    $saved=Get-Content $profile -Raw -Encoding UTF8|ConvertFrom-Json
    if($empty.Count -ne 0 -or $saved.chains.Count -ne 0 -or (Invoke-RestMethod "$base/audio").chain_id){throw 'Last page did not release registry/selection'}
    & "$PSScriptRoot/stop.ps1" -RecordPath $receipt
    if(Get-Process avqm,mediamtx -ErrorAction SilentlyContinue){throw 'First run left media processes'}
    Boot
    if(@(Samples 'restart-empty').Count -ne 0){throw 'Closed pages reappeared after restart'}
    $new=Invoke-RestMethod "$base/chains" -Method Post -Headers $headers -ContentType application/json -Body '{"protocol":"rtmp","alias":"After empty restart"}' -TimeoutSec 2
    $reopened=@(Phase 'reopened' 10)
    if($reopened.Count -ne 1 -or $reopened[0].output_fps -lt 20 -or $new.id -in @('rtsp-tcp',$b.id,$c.id)){throw 'Empty workspace could not create a fresh unique page'}
    Close $new.id
    $null=Samples 'final-empty'
    @{passed=$true;seconds=$watch.Elapsed.TotalSeconds;peak_private_mib=$peak/1MB;server_peak_mib=$serverPeak/1MB;max_cpu_pct=$maxCpu;binary_sha256=(Get-FileHash runtime/avqm.exe -Algorithm SHA256).Hash;middle_id=$b.id;surviving_id=$c.id;new_id=$new.id;surviving_session=$survivor.session;middle_history_before=$historyBefore.Count;middle_history_after=$historyAfter.Count;audio_after_middle=$audio;before=$before;after_middle=$middleClosed;after_first=$one;empty_restart=$true;reopened=$reopened}|ConvertTo-Json -Depth 14|Set-Content (Join-Path $directory 'result.json') -Encoding UTF8
}finally{if($receipt -and (Test-Path $receipt)){& "$PSScriptRoot/stop.ps1" -RecordPath $receipt}}
Write-Output "Close page evidence: $directory"
