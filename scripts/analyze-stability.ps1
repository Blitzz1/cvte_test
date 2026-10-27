param([string]$Directory='',[switch]$Quiet)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
if(-not $Directory){$Directory=(Get-ChildItem artifacts -Directory -Filter 'stability-*'|Sort-Object LastWriteTime -Descending|Select-Object -First 1).FullName}
if(-not $Directory){throw 'No stability collection directory found'}
$rows=@(Get-Content (Join-Path $Directory 'samples.jsonl') -Encoding UTF8|ForEach-Object {$_|ConvertFrom-Json})
if($rows.Count -lt 2){throw 'Need at least two samples'}
function Chains($row){if($row.chains.PSObject.Properties.Name -contains 'value'){@($row.chains.value)}else{@($row.chains)}}
function Summary($values){$v=@($values|Where-Object {$null -ne $_}|Sort-Object);if(-not $v.Count){return $null};@{min=$v[0];median=$v[[int][Math]::Floor(($v.Count-1)/2)];max=$v[-1]}}
$chainResults=@()
$ids=@((Chains $rows[0])|ForEach-Object {$_.id})
foreach($id in $ids){
    $points=@(foreach($row in $rows){foreach($chain in (Chains $row)){if($chain.id -eq $id){$chain}}})
    $chainResults+=@{id=$id;samples=$points.Count;nonrunning_samples=@($points|Where-Object state -NE 'running').Count;zero_fps_samples=@($points|Where-Object output_fps -LE 0).Count;fps_below28_samples=@($points|Where-Object output_fps -LT 28).Count;valid_latency_samples=@($points|Where-Object {$null -ne $_.latency_ms}).Count;valid_sync_samples=@($points|Where-Object {$null -ne $_.av_offset_ms}).Count;unknown_loss_samples=@($points|Where-Object loss_unknown -GT 0).Count;presented_frames_delta=($points[-1].presented_frames-$points[0].presented_frames);source_frames_delta=($points[-1].sent_frames-$points[0].sent_frames);observed_average_output_fps=($points[-1].presented_frames-$points[0].presented_frames)/($rows[-1].elapsed_s-$rows[0].elapsed_s);fps=(Summary @($points|ForEach-Object output_fps));latency_ms=(Summary @($points|ForEach-Object latency_ms));av_offset_ms=(Summary @($points|ForEach-Object av_offset_ms));loss_pct=(Summary @($points|ForEach-Object loss_pct));confirmed_loss_pct=(Summary @($points|ForEach-Object loss_confirmed_pct));rtp_loss_pct=(Summary @($points|ForEach-Object rtp_loss_pct));drift_ms_per_s=(Summary @($points|ForEach-Object drift_ms_per_s));connections=@($points|ForEach-Object receiver_connection|Select-Object -Unique)}
}
$warm=@($rows|Where-Object elapsed_s -GE 300)
if($warm.Count -lt 2){$warm=$rows}
$firstWarm=$warm[0];$last=$rows[-1]
$elapsed=$last.elapsed_s-$rows[0].elapsed_s
$timeline=@();$previous=$null
foreach($row in $rows){
    $c=@(Chains $row);$gap=if($previous){($row.time_ms-$previous.time_ms)/1000}else{0}
    $timeline+=[pscustomobject]@{time_beijing=[DateTimeOffset]::FromUnixTimeMilliseconds($row.time_ms).ToOffset([TimeSpan]::FromHours(8)).ToString('yyyy-MM-dd HH:mm:ss');elapsed_s=$row.elapsed_s;sample_gap_s=$gap;private_mb=$row.private_bytes/1MB;working_set_mb=$row.working_set/1MB;app_cpu_pct=if($gap -gt 0){100*($row.cpu_seconds-$previous.cpu_seconds)/$gap/$row.logical_processors}else{0};min_fps=($c.output_fps|Measure-Object -Minimum).Minimum;max_latency_ms=(@($c|Where-Object {$null -ne $_.latency_ms}|ForEach-Object latency_ms)|Measure-Object -Maximum).Maximum;max_loss_upper_pct=(@($c|Where-Object {$null -ne $_.loss_pct}|ForEach-Object loss_pct)|Measure-Object -Maximum).Maximum;zero_fps_chains=@($c|Where-Object output_fps -LE 0).Count;invalid_latency_chains=@($c|Where-Object {$null -eq $_.latency_ms}).Count;invalid_sync_chains=@($c|Where-Object {$null -eq $_.av_offset_ms}).Count;max_video_queue=($c.video_queue_size|Measure-Object -Maximum).Maximum;fault_active_chains=@($c|Where-Object {$_.fault.delay_ms -gt 0 -or $_.fault.audio_delay_ms -gt 0 -or $_.fault.video_delay_ms -gt 0 -or $_.fault.presentation_drop_pct -gt 0 -or $_.fault.packet_loss_pct -gt 0 -or $_.fault.frame_loss_pct -gt 0 -or $_.fault.drift_ms_per_s -ne 0}).Count}
    $previous=$row
}
$timeline|Export-Csv (Join-Path $Directory 'timeline.csv') -NoTypeInformation -Encoding UTF8
$result=@{directory=$Directory;elapsed_seconds=$elapsed;collection_complete=(Test-Path (Join-Path $Directory 'result.json'));hour_observed=($elapsed -ge 3600);samples=$rows.Count;chains=$chainResults;private_memory_mb=(Summary @($warm|ForEach-Object {$_.private_bytes/1MB}));warm_private_change_mb=($last.private_bytes-$firstWarm.private_bytes)/1MB;handles=(Summary @($warm|ForEach-Object handles));average_app_cpu_pct=100*($last.cpu_seconds-$rows[0].cpu_seconds)/[Math]::Max(1,$elapsed)/$last.logical_processors;average_server_cpu_pct=100*($last.server_cpu_seconds-$rows[0].server_cpu_seconds)/[Math]::Max(1,$elapsed)/$last.logical_processors;note='Descriptive evidence only. No crash or bounded memory growth does not fully prove absence of leaks; review abnormal samples and missing data.'}
if(Test-Path (Join-Path $Directory 'result.json')){$result.poll_result=Get-Content (Join-Path $Directory 'result.json') -Raw -Encoding UTF8|ConvertFrom-Json}
$result.all_zero_fps_samples=@($timeline|Where-Object zero_fps_chains -EQ $ids.Count).Count
$result.any_fps_below20_samples=@($timeline|Where-Object min_fps -LT 20).Count
$result.any_latency_above1000_samples=@($timeline|Where-Object max_latency_ms -GT 1000).Count
$result.fault_active_samples=@($timeline|Where-Object fault_active_chains -GT 0).Count
$result.sampling_gaps_s=Summary @($timeline|Where-Object sample_gap_s -GT 0|ForEach-Object sample_gap_s)
$result.verdict='Review required. Collection completion and process survival are not acceptance; this summary does not establish absence of memory leaks.'
$result|ConvertTo-Json -Depth 12|Set-Content (Join-Path $Directory 'analysis.json') -Encoding UTF8
if(-not $Quiet){$result|ConvertTo-Json -Depth 12}
