param([switch]$Refresh)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$root = Split-Path $PSScriptRoot
$deps = Join-Path $root 'third_party'
$cache = Join-Path $deps 'downloads'
New-Item -ItemType Directory -Force $cache | Out-Null
$lockPath = Join-Path $root 'config/dependencies.lock.json'
$previous = @{}
if (Test-Path $lockPath) { foreach ($entry in (Get-Content $lockPath -Raw | ConvertFrom-Json)) { $previous[$entry.name] = $entry } }
$manifest = @()
function Fetch([string]$name, [string]$url, [string]$file) {
    $target = Join-Path $cache $file
    if (-not (Test-Path $target) -or $Refresh) {
        & curl.exe --silent --show-error --fail --location --retry 3 --connect-timeout 20 --max-time 600 --output "$target.part" $url
        if ($LASTEXITCODE -ne 0) { throw "Download failed: $name" }
        Move-Item -LiteralPath "$target.part" -Destination $target -Force
    }
    $hash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLower()
    if ($previous.ContainsKey($name) -and $previous[$name].sha256 -ne $hash -and -not $Refresh) { throw "Hash mismatch: $name" }
    $script:manifest += [pscustomobject]@{ name=$name; url=$url; file=$file; sha256=$hash }
    return $target
}
$mtx = Fetch 'mediamtx-1.21.3' 'https://github.com/bluenviron/mediamtx/releases/download/v1.21.3/mediamtx_v1.21.3_windows_amd64.zip' 'mediamtx-1.21.3.zip'
if ((Get-FileHash $mtx -Algorithm SHA256).Hash.ToLower() -ne 'b2f5246b07cb72961df03b8d5e8cad43dc657bcf3f49a87bb031bdf22721af74') { throw 'MediaMTX upstream hash mismatch' }
if (-not (Test-Path "$deps/mediamtx/mediamtx.exe")) { Expand-Archive -LiteralPath $mtx -DestinationPath "$deps/mediamtx" -Force }
$ff = Fetch 'ffmpeg-8.1-shared-snapshot' 'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip' 'ffmpeg-8.1-shared.zip'
if (-not (Test-Path "$deps/ffmpeg/include")) {
    Expand-Archive -LiteralPath $ff -DestinationPath "$deps/ffmpeg-extract" -Force
    $dir = Get-ChildItem "$deps/ffmpeg-extract" -Directory | Select-Object -First 1
    New-Item -ItemType Directory -Force "$deps/ffmpeg" | Out-Null
    Copy-Item -Path "$($dir.FullName)/*" -Destination "$deps/ffmpeg" -Recurse -Force
}
$json = Fetch 'nlohmann-json-3.12.0' 'https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp' 'json.hpp'
$http = Fetch 'cpp-httplib-0.28.0' 'https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.28.0/httplib.h' 'httplib.h'
$hls = Fetch 'hls.js-1.6.13' 'https://cdn.jsdelivr.net/npm/hls.js@1.6.13/dist/hls.min.js' 'hls.min.js'
New-Item -ItemType Directory -Force "$deps/include/nlohmann","$root/web/vendor" | Out-Null
Copy-Item $json "$deps/include/nlohmann/json.hpp" -Force
Copy-Item $http "$deps/include/httplib.h" -Force
Copy-Item $hls "$root/web/vendor/hls.min.js" -Force
$sqlite = Fetch 'sqlite-amalgamation-3500400' 'https://sqlite.org/2025/sqlite-amalgamation-3500400.zip' 'sqlite.zip'
if (-not (Test-Path "$deps/sqlite/sqlite3.c")) {
    Expand-Archive $sqlite "$deps/sqlite-extract" -Force
    $dir = Get-ChildItem "$deps/sqlite-extract" -Directory | Select-Object -First 1
    New-Item -ItemType Directory -Force "$deps/sqlite" | Out-Null
    Copy-Item -Path "$($dir.FullName)/*" -Destination "$deps/sqlite" -Recurse -Force
}
New-Item -ItemType Directory -Force "$root/config" | Out-Null
$manifest | ConvertTo-Json -Depth 5 | Set-Content $lockPath -Encoding UTF8
Write-Output 'Dependencies downloaded and SHA-256 locked. Build and runtime will work offline.'
