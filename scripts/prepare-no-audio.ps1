param([string]$Source='data/media/sample-3m.mp4',[string]$Output='data/media/sample-1080p30-no-audio.mp4')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Set-Location $root
if(-not(Test-Path -LiteralPath $Source -PathType Leaf)){throw 'Source file is missing'}
if(Test-Path -LiteralPath $Output){throw 'Output already exists; choose a new path to preserve your existing fixture'}
& "$root/third_party/ffmpeg/bin/ffmpeg.exe" -hide_banner -loglevel error -n -i $Source -map 0:v:0 -c:v copy -an -movflags +faststart $Output
if($LASTEXITCODE){throw 'Video-only fixture generation failed'}
& "$root/third_party/ffmpeg/bin/ffprobe.exe" -v error -show_entries stream=codec_name,width,height,r_frame_rate -of json $Output
if($LASTEXITCODE){throw 'Fixture probe failed'}
Write-Output 'Created a separate video-only file without re-encoding the video; source is unchanged. Set has_audio=false for this chain.'
