param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
if(-not(Test-Path (Join-Path $Directory 'result.json'))){throw 'Wait for collection completion before final review'}
& "$PSScriptRoot/analyze-stability.ps1" -Directory $Directory -Quiet
$analysis=Get-Content (Join-Path $Directory 'analysis.json') -Raw -Encoding UTF8|ConvertFrom-Json
$rows=@(Get-Content (Join-Path $Directory 'samples.jsonl') -Encoding UTF8|ForEach-Object {$_|ConvertFrom-Json})
function FlatChains($row){if($row.chains.PSObject.Properties.Name -contains 'value'){@($row.chains.value)}else{@($row.chains)}}
function Median($values){$sorted=@($values|Sort-Object);if(-not $sorted.Count){return $null};$sorted[[int][Math]::Floor(($sorted.Count-1)/2)]}
$stable=@($rows|Where-Object elapsed_s -ge 30)
if($stable.Count -lt 2){throw 'Insufficient post-startup samples'}
$issues=[Collections.Generic.List[string]]::new()
$collection=Get-Content (Join-Path $Directory 'result.json') -Raw -Encoding UTF8|ConvertFrom-Json
if($collection.aborted){$issues.Add('collection_aborted: '+$collection.abort_reason)}
if($collection.poll_failures){$issues.Add('collection_failures')}
$coverage=([DateTimeOffset]::Parse($collection.finished)-[DateTimeOffset]::Parse($collection.started)).TotalSeconds
$chainReviews=@()
foreach($id in @((FlatChains $stable[0])|ForEach-Object id)){
    $points=@(foreach($row in $stable){foreach($chain in (FlatChains $row)){if($chain.id -eq $id){$chain}}})
    $fpsRatio=100*@($points|Where-Object output_fps -ge 28).Count/$points.Count
    $latencyRatio=100*@($points|Where-Object {$null -ne $_.latency_ms}).Count/$points.Count
    $syncRatio=100*@($points|Where-Object {$null -ne $_.av_offset_ms}).Count/$points.Count
    $sessions=@($points.session|Select-Object -Unique)
    $generations=@($points.playback_generation|Select-Object -Unique)
    $average=if($sessions.Count -eq 1 -and $generations.Count -eq 1){($points[-1].presented_frames-$points[0].presented_frames)/($stable[-1].elapsed_s-$stable[0].elapsed_s)}else{$null}
    $confirmed=@($points|Where-Object loss_confirmed_pct -gt 0).Count
    $unknown=@($points|Where-Object loss_unknown -gt 0).Count
    $dropped=($points.observation_records_dropped|Measure-Object -Maximum).Maximum
    if($null -eq $average -or $average -lt 28 -or $fpsRatio -lt 99 -or @($points|Where-Object output_fps -le 0).Count){$issues.Add("$id`: output_rate_or_epoch_requires_review")}
    if($latencyRatio -lt 95 -or $syncRatio -lt 95){$issues.Add("$id`: metric_validity_requires_review")}
    if($confirmed -or $unknown -or $dropped){$issues.Add("$id`: loss_or_evidence_requires_review")}
    $sourceFirst=$null;$sourceLast=$null;$presentFirst=$null;$presentLast=$null
    $files=@(Get-ChildItem (Join-Path $root "data/observations/$($points[-1].session)*.jsonl")|Where-Object Name -NotLike '*truth*'|Sort-Object LastWriteTime)
    if($files.Count){
        foreach($line in Get-Content $files[0].FullName -TotalCount 2000 -Encoding UTF8){
            try{$event=$line|ConvertFrom-Json}catch{continue}
            if($null -eq $sourceFirst -and $event.stage -eq 'source_release' -and $event.video){$sourceFirst=$event.mono_us}
            if($null -eq $presentFirst -and $event.stage -eq 'software_present'){$presentFirst=$event.mono_us}
        }
        foreach($line in Get-Content $files[-1].FullName -Tail 2000 -Encoding UTF8){
            try{$event=$line|ConvertFrom-Json}catch{continue}
            if($event.stage -eq 'source_release' -and $event.video){$sourceLast=$event.mono_us}
            if($event.stage -eq 'software_present'){$presentLast=$event.mono_us}
        }
    }
    $sourceSpan=if($null -ne $sourceFirst -and $null -ne $sourceLast){($sourceLast-$sourceFirst)/1000000.0}else{$null}
    $presentSpan=if($null -ne $presentFirst -and $null -ne $presentLast){($presentLast-$presentFirst)/1000000.0}else{$null}
    $chainReviews+=@{id=$id;samples=$points.Count;fps_ge28_pct=$fpsRatio;average_output_fps=$average;valid_latency_pct=$latencyRatio;valid_sync_pct=$syncRatio;confirmed_loss_samples=$confirmed;unknown_loss_samples=$unknown;evidence_records_dropped=$dropped;generations=$generations;sessions=$sessions;raw_source_span_s=$sourceSpan;raw_presentation_span_s=$presentSpan;last_uptime_s=$points[-1].uptime_s}
}
$warm=@($rows|Where-Object elapsed_s -ge 300)
$firstWindow=@($warm|Where-Object elapsed_s -lt 600)
$lastWindow=@($warm|Where-Object elapsed_s -ge ($rows[-1].elapsed_s-300))
$firstMedian=Median @($firstWindow|ForEach-Object {$_.private_bytes/1MB})
$lastMedian=Median @($lastWindow|ForEach-Object {$_.private_bytes/1MB})
$slope=$null
if($warm.Count -ge 2){
    $mx=($warm.elapsed_s|Measure-Object -Average).Average/60
    $my=(@($warm|ForEach-Object{$_.private_bytes/1MB})|Measure-Object -Average).Average
    $xx=0.0;$xy=0.0
    foreach($row in $warm){$x=$row.elapsed_s/60-$mx;$xx+=$x*$x;$xy+=$x*($row.private_bytes/1MB-$my)}
    if($xx -gt 0){$slope=$xy/$xx}
}
$growth=if($null -ne $firstMedian -and $null -ne $lastMedian){$lastMedian-$firstMedian}else{$null}
if($null -eq $growth -or $growth -gt 64 -or $null -eq $slope -or $slope -gt .5){$issues.Add('steady_memory_growth_requires_review')}
$rawHours=@($chainReviews|Where-Object {$null -eq $_.raw_source_span_s -or $_.raw_source_span_s -lt 3600}).Count -eq 0
if($coverage -lt 3600 -or ($analysis.elapsed_seconds -lt 3600 -and -not $rawHours)){$issues.Add('insufficient_hour_coverage')}
if($analysis.fault_active_samples){$issues.Add('unexpected_fault_injection')}
$review=@{collection_seconds=$coverage;api_observation_span_s=$analysis.elapsed_seconds;raw_source_hour_covered=$rawHours;startup_excluded_seconds=30;chains=$chainReviews;steady_first5_median_mib=$firstMedian;steady_last5_median_mib=$lastMedian;steady_median_growth_mib=$growth;steady_slope_mib_per_min=$slope;issues=@($issues);verdict=if($issues.Count){'review_required'}else{'criteria_satisfied_pending_final_event_review'};note='Uses predeclared engineering criteria. Review events, cleanup and raw facts before final acceptance; no proof of universal absence of leaks.'}
$review|ConvertTo-Json -Depth 12|Set-Content (Join-Path $Directory 'acceptance-review.json') -Encoding UTF8
$review|ConvertTo-Json -Depth 12
