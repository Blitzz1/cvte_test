$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
$base='http://127.0.0.1:8080/api/v1'
$health=Invoke-RestMethod "$base/health"
$headers=@{'X-AVQM-Token'=$health.token}
function Snapshot([string]$file){(Invoke-WebRequest "$base/chains" -UseBasicParsing).Content | Set-Content "artifacts/$file.json" -Encoding UTF8;Write-Output "Saved $file"}
function Fault([string]$chain,[string]$body){Invoke-RestMethod "$base/chains/$chain/fault-injection" -Method Post -Headers $headers -ContentType 'application/json' -Body $body | Out-Null}
Snapshot 'baseline-current'
Fault 'rtsp-tcp' '{"presentation_drop_pct":20,"seed":7}'
Start-Sleep -Seconds 12
Snapshot 'presentation-drop-current'
Fault 'rtsp-tcp' '{}'
Fault 'rtsp-udp' '{"packet_loss_pct":2,"seed":11}'
Start-Sleep -Seconds 12
Snapshot 'packet-drop-current'
Fault 'rtsp-udp' '{}'
Fault 'rtmp-3' '{"audio_delay_ms":350}'
Start-Sleep -Seconds 12
Snapshot 'audio-delay-current'
Fault 'rtmp-3' '{}'
Start-Sleep -Seconds 12
Snapshot 'recovery-current'
Invoke-RestMethod "$base/config" -Method Patch -Headers $headers -ContentType 'application/json' -Body '{"settings":{"webhook":"http://127.0.0.1:8080/webhook-demo"}}' | Out-Null
Fault 'rtmp-4' '{"video_delay_ms":800}'
Start-Sleep -Seconds 12
Snapshot 'video-delay-current'
Fault 'rtmp-4' '{}'
(Invoke-WebRequest "$base/events" -UseBasicParsing).Content | Set-Content artifacts/events-current.json -Encoding UTF8
Write-Output 'Observations saved; inspect measured outcomes and independent truth logs.'
