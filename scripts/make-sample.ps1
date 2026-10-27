param([int]$Seconds=180,[string]$OutputName='sample.mp4')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
New-Item -ItemType Directory -Force "$root/data/media" | Out-Null
& "$root/third_party/ffmpeg/bin/ffmpeg.exe" -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=1920x1080:rate=30' -f lavfi -i 'sine=frequency=440:sample_rate=48000' -t $Seconds -c:v libx264 -preset ultrafast -crf 26 -maxrate 3M -bufsize 3M -g 30 -bf 0 -pix_fmt yuv420p -c:a aac -b:a 128k -movflags +faststart "$root/data/media/$OutputName"
if($LASTEXITCODE){throw 'sample generation failed'}
Write-Output 'Synthetic transport fixture generated; use your own synchronized ordinary files for final material validation.'
