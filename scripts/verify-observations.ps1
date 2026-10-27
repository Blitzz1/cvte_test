$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
$snapshot=Invoke-RestMethod http://127.0.0.1:8080/api/v1/chains
Start-Sleep -Seconds 2
$checks=@()
foreach($chain in $snapshot){
    if($chain.source_reference -ne 'managed_source_release'){$checks+=@{chain=$chain.id;status='not_applicable';reason='missing_source_reference';passed=$null};continue}
    $facts=@()
    foreach($path in Get-ChildItem "data/observations/$($chain.session)*.jsonl"|Where-Object Name -NotLike '*truth*'){
        foreach($line in Get-Content $path.FullName -Encoding UTF8){
            try{$event=$line|ConvertFrom-Json}catch{continue}
            if($event.stage -eq 'software_present' -and $event.source_us -gt 0 -and $event.mono_us -le $chain.mono_us){$facts+=$event}
        }
    }
    $latest=($facts|Measure-Object mono_us -Maximum).Maximum
    $window=@($facts|Where-Object {$_.mono_us -ge $latest-1000000})
    $values=@($window|ForEach-Object {($_.mono_us-$_.source_us)/1000.0})
    $recomputed=if($values.Count){($values|Measure-Object -Average).Average}else{$null}
    $errorMs=if($null -ne $recomputed -and $null -ne $chain.latency_ms){[Math]::Abs($recomputed-$chain.latency_ms)}else{$null}
    $checks+=@{chain=$chain.id;session=$chain.session;snapshot_mono_us=$chain.mono_us;raw_samples=$values.Count;reported_ms=$chain.latency_ms;recomputed_ms=$recomputed;difference_ms=$errorMs;passed=($null -ne $errorMs -and $errorMs -lt 0.001)}
}
$checks|ConvertTo-Json -Depth 5|Set-Content artifacts/raw-latency-recheck.json -Encoding UTF8
$checks|ConvertTo-Json -Depth 5
if(@($checks|Where-Object {$_.passed -eq $false}).Count){exit 1}
