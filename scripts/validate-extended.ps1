$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
$api='http://127.0.0.1:8080/api/v1'
$headers=@{'X-AVQM-Token'=(Invoke-RestMethod "$api/health").token}
function Post([string]$path,[string]$body){Invoke-RestMethod "$api/$path" -Method Post -Headers $headers -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes($body))|Out-Null}
function Save([string]$name){(Invoke-WebRequest "$api/chains" -UseBasicParsing).Content|Set-Content "artifacts/$name.json" -Encoding UTF8}
$original=Invoke-RestMethod "$api/config"
$updated=$original|ConvertTo-Json -Depth 12|ConvertFrom-Json
foreach($c in $updated.chains){if($c.id -in @('rtsp-tcp','rtmp-3')){$c.source='data/media/sintel-trailer.mp4'}}
Invoke-RestMethod "$api/config" -Method Patch -Headers $headers -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes(($updated|ConvertTo-Json -Depth 12)))|Out-Null
foreach($id in @('rtsp-tcp','rtmp-3')){Post "chains/$id/restart" '{}'}
Post 'chains/chain-5/fault-injection' '{"drift_ms_per_s":5}'
Post 'chains/rtmp-4/fault-injection' '{"presentation_drop_pct":100}'
Start-Sleep -Seconds 5
Save 'freeze-and-ordinary'
Post 'chains/rtmp-4/fault-injection' '{}'
Start-Sleep -Seconds 35
Save 'ordinary-before-loop'
Start-Sleep -Seconds 35
Save 'drift-and-ordinary-loop'
Post 'chains/chain-5/fault-injection' '{}'
(Invoke-WebRequest "$api/events" -UseBasicParsing).Content|Set-Content artifacts/extended-events.json -Encoding UTF8
$current=Invoke-RestMethod "$api/config"
$original.revision=$current.revision
Invoke-RestMethod "$api/config" -Method Patch -Headers $headers -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes(($original|ConvertTo-Json -Depth 12)))|Out-Null
foreach($id in @('rtsp-tcp','rtmp-3')){Post "chains/$id/restart" '{}'}
Write-Output 'Extended observations saved; review actual evidence before marking pass.'
