param([ValidateRange(60,300)][int]$Seconds=240,[string]$Config='config/app.json')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
if(Get-Process avqm -ErrorAction SilentlyContinue){throw 'Do not overlap media runs'}
$directory=Join-Path $root ('artifacts/ui-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $directory|Out-Null
$profile=Join-Path $directory 'app.json'
Copy-Item -LiteralPath $Config -Destination $profile
$receipt=Join-Path $directory 'processes.json'
$watch=[Diagnostics.Stopwatch]::StartNew()
$guard=$false;$previous=$null;$highCpu=0
try{
    & "$PSScriptRoot/start.ps1" -Config $profile -Seconds $Seconds -NoBrowser
    Copy-Item runtime/processes.json $receipt
    $record=Get-Content $receipt -Raw -Encoding UTF8|ConvertFrom-Json
    Write-Output "UI regression evidence: $directory"
    while($watch.Elapsed.TotalSeconds -lt $Seconds){
        $process=Get-Process -Id $record.app_pid -ErrorAction SilentlyContinue
        if(-not $process){break}
        $elapsed=$watch.Elapsed.TotalSeconds
        $cpu=if($previous){100*($process.CPU-$previous.cpu)/[Math]::Max(.1,$elapsed-$previous.elapsed)/[int]$env:NUMBER_OF_PROCESSORS}else{0}
        @{elapsed_s=$elapsed;private_mib=$process.PrivateMemorySize64/1MB;cpu_pct=$cpu}|ConvertTo-Json -Compress|Add-Content (Join-Path $directory 'resources.jsonl') -Encoding UTF8
        $previous=@{cpu=$process.CPU;elapsed=$elapsed}
        if($cpu -gt 20){$highCpu++}else{$highCpu=0}
        if($process.PrivateMemorySize64 -gt 512MB -or $highCpu -ge 2){$guard=$true;Write-Warning 'Resource guard reached';break}
        try{(Invoke-WebRequest 'http://127.0.0.1:8080/api/v1/chains' -UseBasicParsing -TimeoutSec 3).Content|Set-Content (Join-Path $directory 'latest.json') -Encoding UTF8}catch{if(-not(Get-Process -Id $record.app_pid -ErrorAction SilentlyContinue)){break};throw}
        Start-Sleep -Seconds 2
    }
}finally{
    if(Test-Path $receipt){& "$PSScriptRoot/stop.ps1" -RecordPath $receipt}
    @{elapsed_s=$watch.Elapsed.TotalSeconds;resource_guard=$guard;binary_sha256=(Get-FileHash runtime/avqm.exe -Algorithm SHA256).Hash}|ConvertTo-Json|Set-Content (Join-Path $directory 'result.json') -Encoding UTF8
}
if($guard){exit 1}
