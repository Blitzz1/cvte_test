$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$path="$root/runtime/processes.json"
if(-not(Test-Path $path)){Write-Output 'No recorded project processes.';exit 0}
$record=Get-Content $path -Raw | ConvertFrom-Json
$config=Get-Content "$root/config/app.json" -Raw -Encoding UTF8 | ConvertFrom-Json
try{$health=Invoke-RestMethod "http://127.0.0.1:$($config.port)/api/v1/health" -TimeoutSec 2;Invoke-RestMethod "http://127.0.0.1:$($config.port)/api/v1/shutdown" -Method Post -Headers @{'X-AVQM-Token'=$health.token} -TimeoutSec 3 | Out-Null}catch{Write-Output 'API not available; checking recorded process.'}
foreach($kind in @('app','server')){
 $taskPid=$record."${kind}_pid";$expected=[IO.Path]::GetFullPath($record."${kind}_path")
 $process=Get-Process -Id $taskPid -ErrorAction SilentlyContinue
 if($process -and $process.Path -eq $expected){if($kind -eq 'app'){$process.WaitForExit(12000)|Out-Null};if(-not $process.HasExited){Stop-Process -Id $taskPid}}
}
Write-Output 'Project-owned processes stopped.'
