param([int]$Seconds=0,[switch]$NoBrowser,[string]$Config='config/single-chain.json')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
$requestedConfig=$Config
if(-not [IO.Path]::IsPathRooted($requestedConfig)){$requestedConfig=Join-Path $root $requestedConfig}
if(-not(Test-Path "$root/runtime/avqm.exe")){throw 'Build first: powershell -ExecutionPolicy Bypass -File scripts/build.ps1'}
$applicationConfig=Get-Content $requestedConfig -Raw -Encoding UTF8 | ConvertFrom-Json
foreach($chain in $applicationConfig.chains){$sourcePath=if([IO.Path]::IsPathRooted($chain.source)){$chain.source}else{Join-Path $root $chain.source};if($chain.enabled -and $chain.direction -ne 'pull' -and -not(Test-Path $sourcePath)){throw "Source file missing: $($chain.source). Configure your own synchronized H.264/AAC file or run scripts/make-sample.ps1 -OutputName sample-3m.mp4."}}
New-Item -ItemType Directory -Force "$root/artifacts","$root/runtime" | Out-Null
$serverPath=[IO.Path]::GetFullPath("$root/third_party/mediamtx/mediamtx.exe")
$existing=Get-Process mediamtx -ErrorAction SilentlyContinue | Where-Object Path -EQ $serverPath | Select-Object -First 1
if($existing){$server=$existing}else{$server=Start-Process -FilePath $serverPath -ArgumentList 'config/mediamtx.yml' -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput "$root/artifacts/server.log" -RedirectStandardError "$root/artifacts/server-error.log"}
$serverReady=$false
for($attempt=0;$attempt -lt 50;$attempt++){
  if($server.HasExited){throw 'Media server exited; inspect artifacts/server-error.log'}
  $client=[Net.Sockets.TcpClient]::new()
  try{$client.Connect('127.0.0.1',8554);$serverReady=$true;break}catch{Start-Sleep -Milliseconds 100}finally{$client.Dispose()}
}
if(-not $serverReady){throw 'Media server port did not become ready'}
$app=Start-Process -FilePath "$root/runtime/avqm.exe" -ArgumentList '--seconds',$Seconds,'--config',('"'+$requestedConfig+'"') -WorkingDirectory $root -WindowStyle Hidden -PassThru -RedirectStandardOutput "$root/artifacts/app.log" -RedirectStandardError "$root/artifacts/app-error.log"
$record=@{app_pid=$app.Id;server_pid=$server.Id;app_path=[IO.Path]::GetFullPath("$root/runtime/avqm.exe");server_path=$serverPath;port=$applicationConfig.port;app_started_utc=$app.StartTime.ToUniversalTime().ToString('o');server_started_utc=$server.StartTime.ToUniversalTime().ToString('o')}
$record|ConvertTo-Json|Set-Content "$root/runtime/processes.json" -Encoding UTF8
if($Seconds -gt 0){
  New-Item -ItemType Directory -Force "$root/runtime/runs"|Out-Null
  $receipt="$root/runtime/runs/run-$($app.Id)-$([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()).json"
  $record|ConvertTo-Json|Set-Content $receipt -Encoding UTF8
  try{
    Start-Process powershell.exe -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$PSScriptRoot+'\wait-and-stop.ps1"'),'-RecordPath',('"'+$receipt+'"'),'-StopAtUtc',([DateTimeOffset]::UtcNow.AddSeconds($Seconds+2).ToString('o')) -WorkingDirectory $root -WindowStyle Hidden -RedirectStandardOutput "$root/artifacts/timed-cleanup.log" -RedirectStandardError "$root/artifacts/timed-cleanup-error.log"|Out-Null
  }catch{& "$PSScriptRoot/stop.ps1" -RecordPath $receipt;throw}
}
$appReady=$false
for($attempt=0;$attempt -lt 50;$attempt++){if($app.HasExited){throw 'Monitor exited; inspect artifacts/app-error.log'};try{Invoke-RestMethod "http://127.0.0.1:$($applicationConfig.port)/api/v1/health" -TimeoutSec 1 | Out-Null;$appReady=$true;break}catch{Start-Sleep -Milliseconds 100}}
if(-not $appReady){throw 'Dashboard did not become ready; inspect artifacts/app-error.log'}
Write-Output "Dashboard: http://127.0.0.1:$($applicationConfig.port) | app PID $($app.Id), server PID $($server.Id)"
if(-not $NoBrowser){Start-Process "http://127.0.0.1:$($applicationConfig.port)"}
