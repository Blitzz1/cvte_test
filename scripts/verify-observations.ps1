param([string]$SnapshotPath='',[string]$OutputPath='artifacts/raw-latency-recheck.json')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
$snapshot=if($SnapshotPath){Get-Content $SnapshotPath -Raw -Encoding UTF8|ConvertFrom-Json}else{Invoke-RestMethod http://127.0.0.1:8080/api/v1/chains}
if(-not $SnapshotPath){Start-Sleep -Seconds 2}
$checks=@()
foreach($chain in $snapshot){
    if($chain.source_reference -ne 'managed_source_release'){$checks+=@{chain=$chain.id;status='not_applicable';reason='missing_source_reference';passed=$null};continue}
    $facts=@()
    foreach($path in Get-ChildItem "data/observations/$($chain.session)*.jsonl"|Where-Object Name -NotLike '*truth*'){
        foreach($line in Get-Content $path.FullName -Encoding UTF8){
            try{$event=$line|ConvertFrom-Json}catch{continue}
            if($event.stage -eq 'software_present' -and $event.source_us -gt 0 -and $event.mono_us -le $chain.mono_us -and $event.mono_us -ge $chain.mono_us-1000000 -and ($null -eq $chain.playback_generation -or $event.generation -eq $chain.playback_generation)){$facts+=$event}
        }
    }
    $window=@($facts)
    $values=@($window|ForEach-Object {($_.mono_us-$_.source_us)/1000.0})
    $recomputed=if($values.Count){($values|Measure-Object -Average).Average}else{$null}
    $errorMs=if($null -ne $recomputed -and $null -ne $chain.latency_ms){[Math]::Abs($recomputed-$chain.latency_ms)}else{$null}
    $offsets=@();$clockErrors=@()
    foreach($fact in $facts){
        $a=$fact.audio_reference
        if($null -eq $a -or $fact.sync_segment -ne $chain.sync_segment -or $a.generation -ne $chain.playback_generation){continue}
        if($a.device_frequency -le 0 -or $fact.mono_us -lt $a.observed_us -or $fact.mono_us-$a.observed_us -ge 15000 -or $fact.mono_us -ge $a.valid_until_us){throw 'Invalid audio reference recorded as usable'}
        $frame=[Math]::Floor([decimal]$a.device_position*48000/[decimal]$a.device_frequency)
        $media=[decimal]$a.pcm_begin_media_us+[Math]::Truncate(($frame-[decimal]$a.pcm_begin_frame)*1000000/48000)
        $clockErrors+= [double][Math]::Abs($media-[decimal]$a.media_us)
        $offsets+= [double](($media+[decimal]$fact.mono_us-[decimal]$a.observed_us-[decimal]$fact.media_us)/1000)
    }
    $sorted=@($offsets|Sort-Object)
    $sync=if($sorted.Count -ge 3){$sorted[[int][Math]::Floor($sorted.Count/2)]}else{$null}
    $syncError=if($chain.sync_validity -eq 'valid' -and $null -ne $sync){[Math]::Abs($sync-$chain.av_offset_ms)}else{$null}
    $syncPassed=if($chain.sync_validity -eq 'valid'){$null -ne $syncError -and $syncError -lt 0.001 -and @($clockErrors|Where-Object{$_ -gt 0}).Count -eq 0}else{$null}
    $latencyPassed=if($null -ne $chain.latency_ms){$null -ne $errorMs -and $errorMs -lt 0.001}else{$null}
    $checks+=@{chain=$chain.id;session=$chain.session;snapshot_mono_us=$chain.mono_us;raw_samples=$values.Count;reported_ms=$chain.latency_ms;recomputed_ms=$recomputed;difference_ms=$errorMs;latency_passed=$latencyPassed;sync_samples=$sorted.Count;sync_validity=$chain.sync_validity;sync_reported_ms=$chain.av_offset_ms;sync_recomputed_ms=$sync;sync_difference_ms=$syncError;sync_passed=$syncPassed;passed=($latencyPassed -ne $false -and $syncPassed -ne $false);scope='Raw trace consistency; not an independent physical measurement'}
}
$checks|ConvertTo-Json -Depth 5|Set-Content $OutputPath -Encoding UTF8
$checks|ConvertTo-Json -Depth 5
if(@($checks|Where-Object {$_.passed -eq $false}).Count){exit 1}
