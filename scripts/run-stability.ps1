param([ValidateRange(1,120)][int]$Minutes=60,[ValidateRange(1,60)][int]$IntervalSeconds=10)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
foreach($name in 'avqm','mediamtx'){
    if(Get-Process $name -ErrorAction SilentlyContinue|Where-Object {$_.Path -and $_.Path.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)}){throw 'Stop existing project media processes before a new acceptance run'}
}
& "$PSScriptRoot/check-runtime.ps1" -Config 'config/stability-four.json' | Out-Null
if($LASTEXITCODE){throw 'Local dependency preflight failed'}
$config=Get-Content config/stability-four.json -Raw -Encoding UTF8|ConvertFrom-Json
$enabled=@($config.chains|Where-Object enabled)
if($enabled.Count -ne 4 -or @($enabled|Where-Object {$_.direction -eq 'pull' -or $_.has_audio -eq $false}).Count){throw 'Use four complete managed chains with audio for basic acceptance'}
$config|Add-Member -NotePropertyName show_monitor -NotePropertyValue $false -Force
$directory=Join-Path $root ('artifacts/longrun-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$profile=Join-Path $directory 'app.json'
[IO.File]::WriteAllText($profile,($config|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
$receipt=Join-Path $directory 'processes.json'
$exitCode=0
try{
    # Independent watchdog outlives a failed collector; normal collector cleanup ends earlier.
    & "$PSScriptRoot/start.ps1" -Config $profile -Seconds ($Minutes*60+120) -NoBrowser
    Copy-Item runtime/processes.json $receipt
    & "$PSScriptRoot/monitor-stability.ps1" -Minutes $Minutes -IntervalSeconds $IntervalSeconds
    $exitCode=$LASTEXITCODE
}finally{
    if(Test-Path $receipt){& "$PSScriptRoot/stop.ps1" -RecordPath $receipt}
}
exit $exitCode
