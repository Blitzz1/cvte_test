$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$scratch=Join-Path $root 'artifacts/cleanup-test'
New-Item -ItemType Directory -Force $scratch|Out-Null
$idleFile=Join-Path $scratch 'idle.ps1'
[IO.File]::WriteAllText($idleFile,'Start-Sleep -Seconds 60')
$helpers=@()
try{
    for($i=0;$i -lt 3;$i++){$helpers+=Start-Process powershell.exe -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$idleFile+'"') -WindowStyle Hidden -PassThru}
    $receipt=@{app_pid=$helpers[0].Id;app_path=$helpers[0].Path;app_started_utc=$helpers[0].StartTime.ToUniversalTime().ToString('o');server_pid=$helpers[1].Id;server_path=$helpers[1].Path;server_started_utc=$helpers[1].StartTime.ToUniversalTime().ToString('o');port=1}
    $wrong=@{app_pid=$helpers[0].Id;app_path=$helpers[0].Path;app_started_utc=$helpers[0].StartTime.ToUniversalTime().AddMinutes(-1).ToString('o')}
    $wrong|ConvertTo-Json|Set-Content (Join-Path $scratch 'wrong.json') -Encoding UTF8
    & "$root/scripts/stop.ps1" -RecordPath (Join-Path $scratch 'wrong.json') -GraceSeconds 0
    if($helpers[0].HasExited){throw 'Start-time ownership guard failed'}
    $receiptPath=Join-Path $scratch 'run.json'
    $receipt|ConvertTo-Json|Set-Content $receiptPath -Encoding UTF8
    & "$root/scripts/wait-and-stop.ps1" -RecordPath $receiptPath -StopAtUtc ([DateTimeOffset]::UtcNow.AddSeconds(1)) -GraceSeconds 0
    if(-not $helpers[0].HasExited -or -not $helpers[1].HasExited){throw 'Timed cleanup failed'}
    if($helpers[2].HasExited){throw 'Unrelated helper was stopped'}
    Write-Output 'PASS: deadline stops both captured idle helpers; mismatched start time and unrelated helper are protected. No media was started.'
}finally{
    foreach($helper in $helpers){if(-not $helper.HasExited){$helper.Kill();$helper.WaitForExit(5000)|Out-Null};$helper.Dispose()}
}
