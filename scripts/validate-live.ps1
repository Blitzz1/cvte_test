param([int]$Seconds=120)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
New-Item -ItemType Directory -Force artifacts | Out-Null
$app=Start-Process -FilePath "$root/runtime/avqm.exe" -ArgumentList '--seconds',$Seconds -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput "$root/artifacts/live-app.log" -RedirectStandardError "$root/artifacts/live-error.log"
$base='http://127.0.0.1:8080/api/v1'
try {
  for($attempt=0;$attempt -lt 30;$attempt++){try{$health=Invoke-RestMethod "$base/health" -TimeoutSec 2;break}catch{Start-Sleep -Seconds 1}}
  if(-not $health){throw 'HTTP service did not start'}
  $headers=@{'X-AVQM-Token'=$health.token}
  function Save-Snapshot([string]$file) { (Invoke-WebRequest "$base/chains" -UseBasicParsing).Content | Set-Content "artifacts/$file" -Encoding UTF8 }
  Start-Sleep -Seconds 20
  Save-Snapshot 'live-baseline.json'
  Write-Output 'Baseline saved'
  Invoke-RestMethod "$base/chains/rtsp-tcp/fault-injection" -Method Post -Headers $headers -ContentType 'application/json' -Body '{"presentation_drop_pct":20,"seed":7}' | Out-Null
  Start-Sleep -Seconds 15
  Save-Snapshot 'live-presentation-drop.json'
  Invoke-RestMethod "$base/chains/rtsp-tcp/fault-injection" -Method Post -Headers $headers -ContentType 'application/json' -Body '{}' | Out-Null
  Invoke-RestMethod "$base/chains/rtmp-3/fault-injection" -Method Post -Headers $headers -ContentType 'application/json' -Body '{"audio_delay_ms":350}' | Out-Null
  Start-Sleep -Seconds 15
  Save-Snapshot 'live-audio-delay.json'
  Invoke-RestMethod "$base/chains/rtmp-3/fault-injection" -Method Post -Headers $headers -ContentType 'application/json' -Body '{}' | Out-Null
  Start-Sleep -Seconds 15
  Save-Snapshot 'live-recovery.json'
  Invoke-RestMethod "$base/chains/rtsp-udp/fault-injection" -Method Post -Headers $headers -ContentType 'application/json' -Body '{"packet_loss_pct":2,"seed":11}' | Out-Null
  Start-Sleep -Seconds 15
  Save-Snapshot 'live-rtp-loss.json'
  Invoke-RestMethod "$base/chains/rtsp-udp/fault-injection" -Method Post -Headers $headers -ContentType 'application/json' -Body '{}' | Out-Null
  Write-Output 'Live injection/recovery observations saved; inspect actual results before claiming pass.'
} finally { if(-not $app.HasExited){ $app.WaitForExit(($Seconds+10)*1000) | Out-Null } }
