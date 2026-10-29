param([ValidateRange(45,120)][int]$Seconds=90)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
if(Get-Process avqm,mediamtx -ErrorAction SilentlyContinue){throw 'Do not overlap media validation'}
$directory=Join-Path $root ('artifacts/receive-timing-'+(Get-Date -Format yyyyMMdd-HHmmss))
New-Item -ItemType Directory -Force $directory|Out-Null
$config=Get-Content config/stability-four.json -Raw -Encoding UTF8|ConvertFrom-Json
$config.chains=@($config.chains|Select-Object -First 3)
$config.settings.webhook=''
$profile=Join-Path $directory 'app.json'
[IO.File]::WriteAllText($profile,($config|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
$receipt=Join-Path $directory 'processes.json'
$watch=[Diagnostics.Stopwatch]::StartNew();$previous=$null;$high=0;$peak=0;$maxCpu=0
try{
    & "$PSScriptRoot/start.ps1" -Config $profile -Seconds $Seconds -NoBrowser
    Copy-Item runtime/processes.json $receipt
    $record=Get-Content $receipt -Raw -Encoding UTF8|ConvertFrom-Json
    Write-Output "Receive timing evidence: $directory"
    while($watch.Elapsed.TotalSeconds -lt $Seconds-5){
        $ap=Get-Process -Id $record.app_pid -ErrorAction Stop
        $sp=Get-Process -Id $record.server_pid -ErrorAction Stop
        $elapsed=$watch.Elapsed.TotalSeconds
        $peak=[Math]::Max($peak,$ap.PrivateMemorySize64)
        if($ap.PrivateMemorySize64 -gt 512MB -or $sp.PrivateMemorySize64 -gt 256MB){throw 'Memory guard reached'}
        if($previous){$cpu=100*($ap.CPU+$sp.CPU-$previous.cpu)/[Math]::Max(.1,$elapsed-$previous.time)/[int]$env:NUMBER_OF_PROCESSORS;$maxCpu=[Math]::Max($maxCpu,$cpu);if($cpu -gt 20){$high++}else{$high=0};if($high -ge 2){throw 'CPU guard reached'}}
        $previous=@{time=$elapsed;cpu=$ap.CPU+$sp.CPU}
        $response=Invoke-RestMethod 'http://127.0.0.1:8080/api/v1/chains' -TimeoutSec 3
        $all=@(foreach($item in $response){$item})
        @{elapsed_s=$elapsed;chains=$all}|ConvertTo-Json -Depth 14 -Compress|Add-Content (Join-Path $directory 'samples.jsonl') -Encoding UTF8
        Start-Sleep -Seconds 2
    }
    if(@($all|Where-Object {$_.association_sample_count -gt 5 -and $_.pipeline_sample_count -gt 5}).Count -ne 3){throw 'Missing real timing observations'}
    @{passed=$true;seconds=$watch.Elapsed.TotalSeconds;peak_private_mib=$peak/1MB;max_cpu_pct=$maxCpu;binary_sha256=(Get-FileHash runtime/avqm.exe -Algorithm SHA256).Hash;final=$all}|ConvertTo-Json -Depth 14|Set-Content (Join-Path $directory 'result.json') -Encoding UTF8
}finally{if(Test-Path $receipt){& "$PSScriptRoot/stop.ps1" -RecordPath $receipt}}
