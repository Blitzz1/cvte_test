param([Parameter(Mandatory=$true)][string]$RecordPath,[Parameter(Mandatory=$true)][DateTimeOffset]$StopAtUtc,[ValidateRange(0,30)][int]$GraceSeconds=12)
$ErrorActionPreference='Stop'
$record=Get-Content -LiteralPath $RecordPath -Raw -Encoding UTF8|ConvertFrom-Json
try{
    while([DateTimeOffset]::UtcNow -lt $StopAtUtc){
        $process=Get-Process -Id $record.app_pid -ErrorAction SilentlyContinue
        if(-not $process){break}
        if($process.Path -ne $record.app_path){break}
        if($record.app_started_utc -and $process.StartTime.ToUniversalTime().Ticks -ne [DateTimeOffset]::Parse($record.app_started_utc).UtcDateTime.Ticks){break}
        Start-Sleep -Milliseconds 500
    }
}finally{
    & "$PSScriptRoot/stop.ps1" -RecordPath $RecordPath -GraceSeconds $GraceSeconds
}
