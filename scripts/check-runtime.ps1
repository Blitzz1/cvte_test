param([string]$Config='config/single-chain.json',[string]$OutputPath='artifacts/runtime-check.json')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
$checks=@()
foreach($path in 'runtime/avqm.exe','third_party/mediamtx/mediamtx.exe','web/index.html','web/app.js','web/diagnostics.js','web/preview.js','web/style.css','web/vendor/hls.min.js'){
    $checks+=@{path=$path;exists=(Test-Path -LiteralPath $path -PathType Leaf)}
}
$dlls=@()
foreach($dll in Get-ChildItem third_party/ffmpeg/bin -Filter '*.dll'){
    $target=Join-Path 'runtime' $dll.Name
    $dlls+=@{name=$dll.Name;matched=((Test-Path -LiteralPath $target) -and (Get-FileHash $dll.FullName -Algorithm SHA256).Hash -eq (Get-FileHash $target -Algorithm SHA256).Hash)}
}
$configValue=Get-Content -LiteralPath $Config -Raw -Encoding UTF8|ConvertFrom-Json
$sources=@($configValue.chains|Where-Object {$_.enabled -and $_.direction -ne 'pull'}|ForEach-Object {@{path=$_.source;exists=(Test-Path -LiteralPath $_.source -PathType Leaf)}})
$lock=Get-Content config/dependencies.lock.json -Raw -Encoding UTF8|ConvertFrom-Json
$hls=$lock|Where-Object name -eq 'hls.js-1.6.13'
$hlsMatched=(Get-FileHash web/vendor/hls.min.js -Algorithm SHA256).Hash -eq $hls.sha256
$passed=(@($checks|Where-Object {-not $_.exists}).Count -eq 0 -and $dlls.Count -ge 5 -and @($dlls|Where-Object {-not $_.matched}).Count -eq 0 -and @($sources|Where-Object {-not $_.exists}).Count -eq 0 -and $hlsMatched)
$report=@{passed=$passed;files=$checks;runtime_dlls=$dlls;sources=$sources;local_hls_matches_lock=$hlsMatched;binary_sha256=(Get-FileHash runtime/avqm.exe -Algorithm SHA256).Hash;scope='Local dependency readiness only. No downloads, no system network changes, no claim of physical network-disconnection testing.'}
$report|ConvertTo-Json -Depth 6|Set-Content -LiteralPath $OutputPath -Encoding UTF8
$report|ConvertTo-Json -Depth 6
if(-not $passed){exit 1}
