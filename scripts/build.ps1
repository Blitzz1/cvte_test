param([switch]$CoreOnly)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$cmake='C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if(-not(Test-Path $cmake)){$cmake=(Get-Command cmake -ErrorAction Stop).Source}
Set-Location $root
$vs='C:\Program Files\Microsoft Visual Studio\18\Community'
$envLines = & cmd.exe /s /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" -vcvars_ver=14.44 >nul && set"
if($LASTEXITCODE){throw 'Cannot initialize MSVC 14.44 environment'}
foreach($line in $envLines){ if($line -match '^([^=]+)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')} }
$env:PATH="$vs\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;$env:PATH"
if($CoreOnly){ & $cmake -S . -B build-core-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release -DAVQM_BUILD_APP=OFF; if($LASTEXITCODE){exit $LASTEXITCODE}; & $cmake --build build-core-ninja; if($LASTEXITCODE){exit $LASTEXITCODE}; & "$([IO.Path]::GetDirectoryName($cmake))/ctest.exe" --test-dir build-core-ninja --output-on-failure }
else { & $cmake --preset windows-release; if($LASTEXITCODE){exit $LASTEXITCODE}; & $cmake --build --preset windows-release }
exit $LASTEXITCODE
