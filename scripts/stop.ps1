[CmdletBinding(SupportsShouldProcess=$true)]
param([string]$RecordPath='', [ValidateRange(0,30)][int]$GraceSeconds=12)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
if(-not $RecordPath){$RecordPath=Join-Path $root 'runtime/processes.json'}
if(-not(Test-Path -LiteralPath $RecordPath)){Write-Output 'No recorded project processes.';return}
$record=Get-Content -LiteralPath $RecordPath -Raw -Encoding UTF8|ConvertFrom-Json
function Get-OwnedProcess([string]$kind){
    $targetId=$record."${kind}_pid"
    if(-not $targetId){return $null}
    $process=Get-Process -Id $targetId -ErrorAction SilentlyContinue
    if(-not $process){return $null}
    $expected=[IO.Path]::GetFullPath($record."${kind}_path")
    if(-not $process.Path -or [IO.Path]::GetFullPath($process.Path) -ne $expected){Write-Warning "Skipping $kind PID $targetId`: executable does not match.";return $null}
    $created=$record."${kind}_started_utc"
    if($created -and $process.StartTime.ToUniversalTime().Ticks -ne [DateTimeOffset]::Parse($created).UtcDateTime.Ticks){Write-Warning "Skipping $kind PID $targetId`: start time does not match.";return $null}
    return $process
}
foreach($kind in @('app','server')){
    $process=Get-OwnedProcess $kind
    if(-not $process){continue}
    if($kind -eq 'server'){
        $activePath=Join-Path $root 'runtime/processes.json'
        if(Test-Path $activePath){
            $active=Get-Content $activePath -Raw -Encoding UTF8|ConvertFrom-Json
            if($active.app_pid -ne $record.app_pid -and $active.server_pid -eq $record.server_pid){
                $newApp=Get-Process -Id $active.app_pid -ErrorAction SilentlyContinue
                if($newApp -and $newApp.Path -eq $active.app_path){Write-Warning 'Server is in use by a newer run; leaving it running.';continue}
            }
        }
    }
    if(-not $PSCmdlet.ShouldProcess("$kind PID $($process.Id)",'Stop recorded project process')){continue}
    if($kind -eq 'app'){
        $port=if($record.port){[int]$record.port}else{8080}
        try{
            $listener=Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue|Where-Object OwningProcess -EQ $process.Id
            if($listener){$health=Invoke-RestMethod "http://127.0.0.1:$port/api/v1/health" -TimeoutSec 2;Invoke-RestMethod "http://127.0.0.1:$port/api/v1/shutdown" -Method Post -Headers @{'X-AVQM-Token'=$health.token} -TimeoutSec 3|Out-Null}
        }catch{Write-Verbose 'Graceful API shutdown unavailable; bounded process cleanup follows.'}
        if($GraceSeconds -gt 0){$process.WaitForExit($GraceSeconds*1000)|Out-Null}
    }
    if(-not $process.HasExited){$process.Kill();if(-not $process.WaitForExit(5000)){throw "Could not stop $kind PID $($process.Id)"}}
}
Write-Output 'Recorded project process cleanup completed.'
